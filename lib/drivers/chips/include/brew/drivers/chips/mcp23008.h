/**
 * MCP23008 I2C expander. Portable: talks only to hal::I2cBus, so it builds and tests on host.
 */
#pragma once

#include "brew/drivers/interfaces/io.h"

#include <cstdint>

namespace brew::drivers {

/**
 * MCP23008 8-bit expander, used for outputs (relays / LEDs).
 * Writes are cached; if the chip is missing or a write fails, the next write() retries
 * (including re-configuring the chip), so outputs recover by themselves.
 */
class Mcp23008 {
public:
    Mcp23008(hal::I2cBus & bus, uint8_t addr, uint8_t output_mask);

    bool begin();   // false if not answering yet (retried on next write)

    hal::DigitalOut & out(uint8_t pin) { return pins_[pin & 7]; }

    void write(uint8_t pin, bool high);
    bool configured() const { return configured_; }

private:
    bool flush();

    struct Pin final : hal::DigitalOut {
        Mcp23008 * chip = nullptr;
        uint8_t n = 0;
        void write(bool v) override { chip->write(n, v); }
    };

    hal::I2cBus & bus_;
    uint8_t addr_;
    uint8_t outputs_;
    uint8_t want_ = 0;
    int have_ = -1;   // last latch value written, -1 = unknown
    bool configured_ = false;
    Pin pins_[8];
};

}  // namespace brew::drivers
