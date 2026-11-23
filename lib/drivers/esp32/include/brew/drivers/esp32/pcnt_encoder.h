/**
 * Quadrature encoder on the PCNT peripheral. ESP-IDF APIs only (no Arduino).
 */
#pragma once

#include "brew/drivers/interfaces/input.h"

#include <cstddef>
#include <cstdint>

namespace brew::drivers {

/**
 * Quadrature encoder decoded x4 in hardware by the PCNT peripheral, with a glitch filter
 * and the internal pull-ups on. count() increases clockwise for the usual A/B wiring;
 * set `reverse` if yours counts the other way.
 */
class EspPcntEncoder final : public hal::Encoder {
public:
    struct Config {
        int      pin_a = -1;
        int      pin_b = -1;
        uint32_t glitch_ns = 1000;
        bool     reverse = false;
    };

    explicit EspPcntEncoder(const Config & cfg) : cfg_(cfg) {}
    bool begin() override;
    int32_t count() override;

private:
    Config cfg_;
    void * unit_ = nullptr;   // pcnt_unit_handle_t
};

}  // namespace brew::drivers
