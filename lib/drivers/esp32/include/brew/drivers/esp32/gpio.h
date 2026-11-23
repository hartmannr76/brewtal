/**
 * GPIO pins as hal::DigitalOut / DigitalIn. ESP-IDF APIs only (no Arduino).
 */
#pragma once

#include "brew/drivers/interfaces/io.h"

#include <cstddef>
#include <cstdint>

namespace brew::drivers {

class EspGpioOut final : public hal::DigitalOut {
public:
    explicit EspGpioOut(int pin, bool initial = false) : pin_(pin), initial_(initial) {}
    bool begin();
    void write(bool high) override;

private:
    int pin_;
    bool initial_;
};

class EspGpioIn final : public hal::DigitalIn {
public:
    explicit EspGpioIn(int pin, bool pullup = true) : pin_(pin), pullup_(pullup) {}
    bool begin();
    bool read() override;

private:
    int pin_;
    bool pullup_;
};

}  // namespace brew::drivers
