#include "brew/drivers/esp32/http_server.h"
#include "brew/drivers/esp32/log.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstdio>
#include <cstring>

namespace brew::drivers {

namespace {
const char * TAG = "brew.http";

esp_err_t handler(httpd_req_t * req)
{
    return static_cast<EspHttpServer *>(req->user_ctx)->serve(req) == 0 ? ESP_OK : ESP_FAIL;
}

const char * status_line(int s)
{
    switch(s) {
        case 200: return "200 OK";
        case 204: return "204 No Content";
        case 302: return "302 Found";
        case 400: return "400 Bad Request";
        case 404: return "404 Not Found";
        default:  return "503 Service Unavailable";
    }
}
}  // namespace

bool EspHttpServer::begin()
{
    if(server_) return true;
    done_ = xSemaphoreCreateBinary();
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = cfg_.port;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.lru_purge_enable = true;   // phones open several sockets; recycle the oldest
    c.stack_size = 6144;
    httpd_handle_t h = nullptr;
    const esp_err_t e = httpd_start(&h, &c);
    if(e != ESP_OK) {
        BREW_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e));
        return false;
    }
    server_ = h;
    BREW_LOGI(TAG, "setup page server listening on port %u", cfg_.port);
    for(const httpd_method_t m : { HTTP_GET, HTTP_POST }) {
        httpd_uri_t u = {};
        u.uri = "/*";
        u.method = m;
        u.handler = handler;
        u.user_ctx = this;
        httpd_register_uri_handler(h, &u);
    }
    return true;
}

int EspHttpServer::serve(void * r)
{
    httpd_req_t * req = static_cast<httpd_req_t *>(r);
    method_ = req->method == HTTP_POST ? "POST" : "GET";
    path_.assign(req->uri);
    const size_t q = path_.find('?');
    if(q != std::string::npos) path_.resize(q);
    body_.clear();
    size_t left = req->content_len < cfg_.max_body ? req->content_len : cfg_.max_body;
    char chunk[256];
    while(left > 0) {
        const int got = httpd_req_recv(req, chunk, left < sizeof(chunk) ? left : sizeof(chunk));
        if(got <= 0) break;
        body_.append(chunk, static_cast<size_t>(got));
        left -= static_cast<size_t>(got);
    }

    BREW_LOGI(TAG, "%s %s", method_.c_str(), req->uri);
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(done_), 0);   // drop a stale give
    state_ = kWaiting;
    const bool answered = xSemaphoreTake(static_cast<SemaphoreHandle_t>(done_), pdMS_TO_TICKS(cfg_.timeout_ms)) == pdTRUE;
    int expected = kWaiting;
    if(!answered && state_.compare_exchange_strong(expected, kIdle)) {   // nobody came: give up
        BREW_LOGW(TAG, "no answer within %u ms: 503", static_cast<unsigned>(cfg_.timeout_ms));
        httpd_resp_set_status(req, status_line(503));
        httpd_resp_send(req, "busy", 4);
        return 0;
    }
    if(!answered) xSemaphoreTake(static_cast<SemaphoreHandle_t>(done_), portMAX_DELAY);   // respond() won the race

    httpd_resp_set_status(req, status_line(status_));
    httpd_resp_set_type(req, type_.c_str());
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if(!location_.empty()) httpd_resp_set_hdr(req, "Location", location_.c_str());
    const esp_err_t e = httpd_resp_send(req, out_.data(), static_cast<ssize_t>(out_.size()));
    state_ = kIdle;
    return e == ESP_OK ? 0 : -1;
}

bool EspHttpServer::pending(hal::HttpRequest & out)
{
    if(state_.load() != kWaiting) return false;
    out.method = method_.c_str();
    out.path = path_.c_str();
    out.body = body_.c_str();
    out.body_len = body_.size();
    return true;
}

void EspHttpServer::respond(const hal::HttpResponse & res)
{
    int expected = kWaiting;
    if(state_.load() != kWaiting) return;
    status_ = res.status;
    type_ = res.content_type ? res.content_type : "text/plain";
    out_.assign(res.body ? res.body : "", res.body_len);
    location_ = res.location ? res.location : "";
    if(state_.compare_exchange_strong(expected, kAnswered)) xSemaphoreGive(static_cast<SemaphoreHandle_t>(done_));
}

}  // namespace brew::drivers
