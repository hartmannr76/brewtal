/**
 * DS18B20 temperature probe, one per GPIO, bit-banged 1-Wire (no extra components needed).
 * Non-blocking: update() starts a conversion, and collects it 750 ms later. Three failed
 * reads in a row mark the reading invalid, which makes the controller switch outputs off.
 *
 * Needs a 4.7 k pull-up to 3V3 on the data line for cable runs; the internal pull-up only
 * works for very short wires.
 */
#pragma once

#include "brew/drivers/interfaces/sensor.h"

namespace brew::drivers {

class EspDs18b20 final : public hal::TempSensor {
public:
    struct Config {
        int      pin = -1;
        uint32_t period_ms = 800;     // a 12-bit conversion takes 750 ms: about as fast as it goes
        float    offset_c = 0.0f;     // calibration
        bool     internal_pullup = true;
        const char * label = "ds18b20";
    };

    explicit EspDs18b20(const Config & cfg) : cfg_(cfg) {}

    bool begin() override;
    void update(uint32_t now_ms) override;
    hal::TempReading latest() const override { return reading_; }
    const char * name() const override { return cfg_.label; }

private:
    enum class State : uint8_t { Idle, Converting };

    bool reset();
    void writeByte(uint8_t v);
    uint8_t readByte();

    Config cfg_;
    State state_ = State::Idle;
    bool started_ = false;
    uint32_t last_start_ = 0;
    int errors_ = 0;
    hal::TempReading reading_;
};

}  // namespace brew::drivers
