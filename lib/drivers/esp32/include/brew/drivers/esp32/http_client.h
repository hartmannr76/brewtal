/**
 * esp_http_client as a hal::HttpClient: one request at a time on its own task, so the caller only
 * starts and polls. HTTPS uses ESP-IDF's certificate bundle.
 *
 * TLS memory: the framework allocates mbedTLS buffers from internal RAM, and a handshake does not
 * fit beside the display and WiFi (measured: mbedtls_ssl_setup fails with -0x7F00). begin()
 * moves mbedTLS's allocations to PSRAM for the whole firmware; the task's stack is in PSRAM too.
 */
#pragma once

#include "brew/drivers/interfaces/http.h"

#include <atomic>
#include <cstddef>
#include <string>

namespace brew::drivers {

class EspHttpClient final : public hal::HttpClient {
public:
    struct Config {
        uint32_t timeout_ms = 20000;
        size_t max_body = 64 * 1024;   // longer responses are cut (truncated = true)
        uint32_t stack = 12288;
    };

    EspHttpClient() = default;
    explicit EspHttpClient(const Config & cfg) : cfg_(cfg) {}

    bool begin() override;
    bool start(const hal::HttpClientRequest & req) override;
    bool poll(hal::HttpClientResult & out) override;

private:
    enum : int { kIdle = 0, kQueued = 1, kDone = 2 };
    static void taskEntry(void * self);
    void run();

    Config cfg_;
    void * wake_ = nullptr;   // SemaphoreHandle_t
    std::atomic<int> state_{ kIdle };
    /* the request (written by start(), read by the task) ... */
    std::string method_, url_, auth_, type_, body_;
    /* ... and its result (written by the task, read by poll()) */
    int status_ = 0;
    std::string resp_;
    bool truncated_ = false;
};

}  // namespace brew::drivers
