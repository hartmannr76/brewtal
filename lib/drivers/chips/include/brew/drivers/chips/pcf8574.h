/**
 * PCF8574 / PCF8574A I2C expander. Portable: talks only to hal::I2cBus, so it builds and tests on host.
 */
#pragma once

#include "brew/drivers/interfaces/io.h"

#include <cstdint>

namespace brew::drivers {

/**
 * PCF8574 / PCF8574A 8-bit quasi-bidirectional expander (100 kHz max).
 * Inputs are pins driven high (weak pull-up); `input_mask` marks them.
 */
class Pcf8574 {
public:
    Pcf8574(hal::I2cBus & bus, uint8_t addr, uint8_t input_mask);

    /** Writes the initial latch: inputs high, outputs low. */
    bool begin();

    hal::DigitalOut & out(uint8_t pin) { return pins_[pin & 7]; }
    hal::DigitalIn & in(uint8_t pin) { return pins_[pin & 7]; }

    bool write(uint8_t pin, bool high);
    bool read(uint8_t pin, bool & high);

private:
    struct Pin final : hal::DigitalOut, hal::DigitalIn {
        Pcf8574 * chip = nullptr;
        uint8_t n = 0;
        bool last = true;
        void write(bool v) override { chip->write(n, v); }
        bool read() override
        {
            bool v;
            if(chip->read(n, v)) last = v;
            return last;   // keep the previous level across a bus glitch
        }
    };

    hal::I2cBus & bus_;
    uint8_t addr_;
    uint8_t inputs_;
    uint8_t latch_;
    Pin pins_[8];
};

}  // namespace brew::drivers
