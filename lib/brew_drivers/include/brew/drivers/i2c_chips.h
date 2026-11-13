/**
 * I2C chip drivers. Portable: they only talk to hal::I2cBus, so they build and test on host.
 */
#pragma once

#include "brew/hal/input.h"
#include "brew/hal/io.h"

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

/**
 * Hynitron CST8xx capacitive touch (CST816S/T/D, CST820, CST826). Same register map:
 * 0x02 = finger count, 0x03.. = XH XL YH YL (12-bit coordinates), 0xAA = chip id.
 */
class Cst8xxTouch final : public hal::Touch {
public:
    struct Config {
        uint8_t  addr = 0x15;
        uint16_t width = 480, height = 480;   // clamp range
        int16_t  x_offset = 0, y_offset = 0;  // added after swap/mirror
        bool     swap_xy = false, mirror_x = false, mirror_y = false;
        /* CST816 parts sleep (and NACK) when nothing touches them, so a failed read means
         * "released" rather than "bus error". */
        bool     nack_means_released = true;
    };

    Cst8xxTouch(hal::I2cBus & bus, const Config & cfg) : bus_(bus), cfg_(cfg) {}

    bool begin() override;
    bool read(hal::TouchPoint & out) override;
    uint8_t chipId() const { return chip_id_; }

private:
    hal::I2cBus & bus_;
    Config cfg_;
    uint8_t chip_id_ = 0;
};

}  // namespace brew::drivers
