/**
 * esp_http_server as a hal::HttpServer. Every GET and POST (any path) is parked for the caller:
 * the server's thread waits up to timeout_ms while the caller's task picks it up with pending()
 * and answers with respond(). One request at a time (esp_http_server runs one handler at once).
 */
#pragma once

#include "brew/drivers/interfaces/http.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace brew::drivers {

class EspHttpServer final : public hal::HttpServer {
public:
    struct Config {
        uint16_t port = 80;
        uint32_t timeout_ms = 3000;   // no answer by then: 503
        size_t   max_body = 1024;     // longer POST bodies are cut
    };

    EspHttpServer() = default;
    explicit EspHttpServer(const Config & cfg) : cfg_(cfg) {}

    bool begin() override;
    bool pending(hal::HttpRequest & out) override;
    void respond(const hal::HttpResponse & res) override;

    /* called on the server's thread */
    int serve(void * req);

private:
    enum : int { kIdle = 0, kWaiting = 1, kAnswered = 2 };

    Config cfg_;
    void * server_ = nullptr;   // httpd_handle_t
    void * done_ = nullptr;     // SemaphoreHandle_t: given by respond()
    std::atomic<int> state_{ kIdle };

    /* the parked request ... */
    std::string method_, path_, body_;
    /* ... and its answer (copied, so the caller's buffers can change right after respond()) */
    int status_ = 200;
    std::string type_, out_, location_;
};

}  // namespace brew::drivers
