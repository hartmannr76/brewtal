/**
 * Wall-clock time (e.g. set from the internet once there is WiFi). Until it has been set,
 * valid() is false and callers keep using whatever they had before.
 */
#pragma once

#include <cstdint>

namespace brew::hal {

class WallClock {
public:
    virtual ~WallClock() = default;
    virtual bool begin() = 0;
    virtual bool valid() = 0;
    /** Seconds since 1970-01-01 UTC; meaningful only while valid(). */
    virtual int64_t unixSeconds() = 0;
};

}  // namespace brew::hal
