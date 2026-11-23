/**
 * PWM backlight on the LEDC peripheral. ESP-IDF APIs only (no Arduino).
 */
#pragma once

#include "brew/drivers/interfaces/display.h"

#include <cstddef>
#include <cstdint>

namespace brew::drivers {

class EspLedcBacklight final : public hal::Backlight {
public:
    struct Config {
        int      pin = -1;
        uint32_t freq_hz = 5000;
        int      timer = 0;      // LEDC timer / channel to claim
        int      channel = 0;
        bool     active_low = false;
    };

    explicit EspLedcBacklight(const Config & cfg) : cfg_(cfg) {}
    bool begin();
    void set(uint8_t level) override;

private:
    Config cfg_;
    bool ready_ = false;
};

}  // namespace brew::drivers
