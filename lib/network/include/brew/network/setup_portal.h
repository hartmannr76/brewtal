/**
 * The setup page's web server logic, independent of any HTTP library: the ESP32 driver and the
 * web harness both turn a request into an HttpRequest, call handle(), and send the HttpResponse.
 *
 *   GET  /             the setup page (lib/network/web/setup.html, embedded at build time)
 *   GET  /api/state    JSON: phase, error, networks, ...
 *   POST /api/scan     look for networks again
 *   POST /api/connect  form fields ssid, password: try that network
 *   POST /api/skip     close setup ("Set up later")
 *   GET  anything else 302 to the page. Phones probe a few fixed URLs to see whether a network
 *                      has internet; a redirect is what makes them pop the setup page open.
 *
 * No heap: JSON is written into a buffer owned by the portal, valid until the next handle().
 */
#pragma once

#include "brew/drivers/interfaces/http.h"
#include "brew/network/wifi_setup.h"

#include <cstddef>
#include <cstdint>

namespace brew::net {

using HttpRequest = hal::HttpRequest;
using HttpResponse = hal::HttpResponse;

class SetupPortal {
public:
    /** `home` is the page's address on the setup network, for the redirect. */
    SetupPortal(WifiSetup & setup, const char * home = "http://192.168.4.1/") : setup_(setup), home_(home) {}

    void handle(const HttpRequest & req, uint32_t now_ms, HttpResponse & res);
    /** Answers every request the server has waiting (call from the task that owns `setup`). */
    void serve(hal::HttpServer & server, uint32_t now_ms);

    /** Decodes form field `name` from an x-www-form-urlencoded body. False if it isn't there. */
    static bool formField(const char * body, size_t len, const char * name, char * out, size_t n);

private:
    void json(HttpResponse & res, int status);
    void stateJson();
    void put(const char * s);
    void putEscaped(const char * s);

    WifiSetup & setup_;
    const char * home_;
    char buf_[3072];
    size_t len_ = 0;
};

/** The embedded setup page (generated: tools/gen_setup_page.py). */
extern const char kSetupPage[];
extern const size_t kSetupPageLen;

}  // namespace brew::net
