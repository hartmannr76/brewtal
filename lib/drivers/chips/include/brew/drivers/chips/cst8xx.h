/**
 * Hynitron CST8xx capacitive touch. Portable: talks only to hal::I2cBus, so it builds and tests on host.
 */
#pragma once

#include "brew/drivers/interfaces/input.h"
#include "brew/drivers/interfaces/io.h"

#include <cstdint>

namespace brew::drivers {

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
