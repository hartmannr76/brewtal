#include "brew/network/setup_portal.h"

#include <cstdio>
#include <cstring>

namespace brew::net {

namespace {
bool is(const char * a, const char * b) { return std::strcmp(a, b) == 0; }

int hexval(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

bool SetupPortal::formField(const char * body, size_t len, const char * name, char * out, size_t n)
{
    if(!body || !name || !out || n == 0) return false;
    const size_t nl = std::strlen(name);
    size_t i = 0;
    while(i < len) {
        size_t e = i;
        while(e < len && body[e] != '&') e++;
        if(e - i > nl && std::strncmp(body + i, name, nl) == 0 && body[i + nl] == '=') {
            size_t o = 0;
            for(size_t k = i + nl + 1; k < e && o + 1 < n; k++) {
                char c = body[k];
                if(c == '+') c = ' ';
                else if(c == '%' && k + 2 < e && hexval(body[k + 1]) >= 0 && hexval(body[k + 2]) >= 0) {
                    c = static_cast<char>(hexval(body[k + 1]) * 16 + hexval(body[k + 2]));
                    k += 2;
                }
                out[o++] = c;
            }
            out[o] = '\0';
            return true;
        }
        i = e + 1;
    }
    return false;
}

void SetupPortal::put(const char * s)
{
    while(*s && len_ + 1 < sizeof(buf_)) buf_[len_++] = *s++;
    buf_[len_] = '\0';
}

void SetupPortal::putEscaped(const char * s)
{
    put("\"");
    for(; *s; s++) {
        const unsigned char c = static_cast<unsigned char>(*s);
        char e[8];
        if(c == '"' || c == '\\') { e[0] = '\\'; e[1] = static_cast<char>(c); e[2] = '\0'; }
        else if(c < 0x20) std::snprintf(e, sizeof(e), "\\u%04x", c);
        else { e[0] = static_cast<char>(c); e[1] = '\0'; }
        put(e);
    }
    put("\"");
}

void SetupPortal::json(HttpResponse & res, int status)
{
    res.status = status;
    res.content_type = "application/json";
    res.body = buf_;
    res.body_len = len_;
}

void SetupPortal::stateJson()
{
    const SetupStatus & s = setup_.status();
    char num[96];
    put("{\"phase\":");
    putEscaped(phaseName(s.phase));
    put(",\"error\":");
    putEscaped(errorName(s.error));
    put(",\"ssid\":");
    putEscaped(s.ssid);
    put(",\"device\":");
    putEscaped(s.ap_ssid);
    std::snprintf(num, sizeof(num), ",\"scanning\":%s,\"setup_left_s\":%u,\"networks\":[",
                  s.scanning ? "true" : "false", static_cast<unsigned>(s.setup_left_ms / 1000u));
    put(num);
    const hal::WifiNetwork * nets;
    const int n = setup_.networks(nets);
    for(int i = 0; i < n; i++) {
        put(i ? ",{\"ssid\":" : "{\"ssid\":");
        putEscaped(nets[i].ssid);
        std::snprintf(num, sizeof(num), ",\"rssi\":%d,\"secured\":%s}", nets[i].rssi, nets[i].secured ? "true" : "false");
        put(num);
    }
    put("]}");
}

void SetupPortal::serve(hal::HttpServer & server, uint32_t now)
{
    HttpRequest req;
    while(server.pending(req)) {
        HttpResponse res;
        handle(req, now, res);
        server.respond(res);
    }
}

void SetupPortal::handle(const HttpRequest & req, uint32_t now, HttpResponse & res)
{
    res = HttpResponse{};
    len_ = 0;
    buf_[0] = '\0';
    const char * m = req.method ? req.method : "GET";
    const char * p = req.path ? req.path : "/";

    if(is(m, "GET") && (is(p, "/") || is(p, "/index.html"))) {
        res.content_type = "text/html; charset=utf-8";
        res.body = kSetupPage;
        res.body_len = kSetupPageLen;
        return;
    }
    if(is(m, "GET") && is(p, "/api/state")) {
        stateJson();
        json(res, 200);
        return;
    }
    if(is(m, "POST") && is(p, "/api/scan")) {
        setup_.rescan();
        put("{\"ok\":true}");
        json(res, 200);
        return;
    }
    if(is(m, "POST") && is(p, "/api/connect")) {
        char ssid[33], pw[65];
        const bool have = formField(req.body, req.body_len, "ssid", ssid, sizeof(ssid));
        if(!formField(req.body, req.body_len, "password", pw, sizeof(pw))) pw[0] = '\0';
        if(!have || !setup_.submit(ssid, pw, now)) {
            put("{\"ok\":false,\"error\":\"Setup is closed, or that network name isn't valid.\"}");
            json(res, 400);
            return;
        }
        put("{\"ok\":true}");
        json(res, 200);
        return;
    }
    if(is(m, "POST") && is(p, "/api/skip")) {
        setup_.cancelSetup(now);
        put("{\"ok\":true}");
        json(res, 200);
        return;
    }
    if(is(m, "GET")) {   // captive-portal probes and anything else: send them to the page
        res.status = 302;
        res.location = home_;
        return;
    }
    res.status = 404;
}

}  // namespace brew::net
