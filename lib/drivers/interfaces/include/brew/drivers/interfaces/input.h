/**
 * Raw input devices. Policy (debounce, detents, press/turn ordering, touch jitter) lives in
 * lib/core (KnobInput, TouchTracker) so every board gets it and it is unit-tested on host.
 */
#pragma once

#include <cstdint>

namespace brew::hal {

/** Quadrature encoder: raw counts, monotonic, never lost (hardware counter or ISR). */
class Encoder {
public:
    virtual ~Encoder() = default;
    virtual bool    begin() = 0;
    virtual int32_t count() = 0;
};

struct TouchPoint {
    int16_t x = 0, y = 0;
    bool pressed = false;
};

/** Touch controller, polled. Offsets/rotation/clamping are driver config. */
class Touch {
public:
    virtual ~Touch() = default;
    virtual bool begin() = 0;
    virtual bool read(TouchPoint & out) = 0;   // false = bus error (out untouched)
};

}  // namespace brew::hal
