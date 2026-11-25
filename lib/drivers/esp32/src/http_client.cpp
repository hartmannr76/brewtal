#include "brew/drivers/esp32/http_client.h"
#include "brew/drivers/esp32/log.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform.h"

namespace brew::drivers {

namespace {
const char * TAG = "brew.httpc";

void * psram_calloc(size_t n, size_t size)
{
    void * p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

esp_http_client_method_t method_of(const std::string & m)
{
    if(m == "POST") return HTTP_METHOD_POST;
    if(m == "PATCH") return HTTP_METHOD_PATCH;
    if(m == "PUT") return HTTP_METHOD_PUT;
    if(m == "DELETE") return HTTP_METHOD_DELETE;
    return HTTP_METHOD_GET;
}
}  // namespace

bool EspHttpClient::begin()
{
    if(wake_) return true;
    /* TLS buffers in PSRAM: internal RAM can't hold a handshake next to the display and WiFi */
    mbedtls_platform_set_calloc_free(psram_calloc, heap_caps_free);
    wake_ = xSemaphoreCreateBinary();
    if(!wake_) return false;
    if(xTaskCreatePinnedToCoreWithCaps(taskEntry, "httpc", cfg_.stack, this, 3, nullptr, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        BREW_LOGE(TAG, "could not start the HTTP client task");
        return false;
    }
    return true;
}

bool EspHttpClient::start(const hal::HttpClientRequest & req)
{
    if(!wake_ || state_.load() != kIdle) return false;
    method_ = req.method ? req.method : "GET";
    url_ = req.url ? req.url : "";
    auth_ = req.authorization ? req.authorization : "";
    type_ = req.content_type ? req.content_type : "";
    body_.assign(req.body ? req.body : "", req.body ? req.body_len : 0);
    state_ = kQueued;
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(wake_));
    return true;
}

bool EspHttpClient::poll(hal::HttpClientResult & out)
{
    if(state_.load() != kDone) return false;
    out.status = status_;
    out.body = resp_.c_str();
    out.body_len = resp_.size();
    out.truncated = truncated_;
    state_ = kIdle;   // the body stays valid until the next start() replaces it
    return true;
}

void EspHttpClient::taskEntry(void * self) { static_cast<EspHttpClient *>(self)->run(); }

void EspHttpClient::run()
{
    for(;;) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(wake_), portMAX_DELAY);
        if(state_.load() != kQueued) continue;
        resp_.clear();
        truncated_ = false;
        status_ = -1;
        esp_http_client_config_t c = {};
        c.url = url_.c_str();
        c.method = method_of(method_);
        c.crt_bundle_attach = esp_crt_bundle_attach;
        c.timeout_ms = static_cast<int>(cfg_.timeout_ms);
        c.buffer_size = 2048;
        c.buffer_size_tx = 1024;
        esp_http_client_handle_t h = esp_http_client_init(&c);
        if(h) {
            if(!auth_.empty()) esp_http_client_set_header(h, "Authorization", auth_.c_str());
            if(!type_.empty()) esp_http_client_set_header(h, "Content-Type", type_.c_str());
            const esp_err_t e = esp_http_client_open(h, static_cast<int>(body_.size()));
            if(e == ESP_OK) {
                if(!body_.empty()) esp_http_client_write(h, body_.data(), static_cast<int>(body_.size()));
                esp_http_client_fetch_headers(h);
                status_ = esp_http_client_get_status_code(h);
                char buf[1024];
                int n;
                while((n = esp_http_client_read(h, buf, sizeof(buf))) > 0) {
                    const size_t room = cfg_.max_body > resp_.size() ? cfg_.max_body - resp_.size() : 0;
                    if(static_cast<size_t>(n) > room) truncated_ = true;
                    resp_.append(buf, static_cast<size_t>(n) < room ? static_cast<size_t>(n) : room);
                }
            }
            else {
                BREW_LOGW(TAG, "%s %s: %s", method_.c_str(), url_.c_str(), esp_err_to_name(e));
            }
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
        }
        state_ = kDone;
    }
}

}  // namespace brew::drivers
