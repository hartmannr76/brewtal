/**
 * Temperature sensors. Reads never block: slow parts (a DS18B20 conversion takes 750 ms)
 * run their own small state machine inside update().
 */
#pragma once

#include <cstdint>

namespace brew::hal {

struct TempReading {
    float    celsius = 0.0f;
    bool     valid = false;
    uint32_t at_ms = 0;   // when it was measured
};

class TempSensor {
public:
    virtual ~TempSensor() = default;
    virtual bool        begin() = 0;
    virtual void        update(uint32_t now_ms) = 0;   // start or collect a measurement
    virtual TempReading latest() const = 0;
    virtual const char * name() const = 0;
    /** True for a simulated fermenter (demo builds): the display says so. */
    virtual bool        simulated() const { return false; }
};

}  // namespace brew::hal
