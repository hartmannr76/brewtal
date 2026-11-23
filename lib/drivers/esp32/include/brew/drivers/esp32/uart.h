/**
 * UART as a hal::ByteStream. ESP-IDF APIs only (no Arduino).
 */
#pragma once

#include "brew/drivers/interfaces/io.h"

#include <cstddef>
#include <cstdint>

namespace brew::drivers {

class EspUartStream final : public hal::ByteStream {
public:
    struct Config {
        int      port = 1;
        int      tx = -1, rx = -1;
        uint32_t baud = 115200;
        size_t   rx_buffer = 1024;
    };

    explicit EspUartStream(const Config & cfg) : cfg_(cfg) {}
    bool begin() override;
    size_t write(const uint8_t * d, size_t n) override;
    size_t read(uint8_t * buf, size_t max) override;

private:
    Config cfg_;
    bool ready_ = false;
};

}  // namespace brew::drivers
