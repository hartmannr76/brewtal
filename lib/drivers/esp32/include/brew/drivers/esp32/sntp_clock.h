/**
 * Wall-clock time from the internet (SNTP, ESP-IDF's client) as a hal::WallClock. It keeps
 * trying in the background and becomes valid() once a server has answered, which needs WiFi.
 * Start it after the WiFi driver (it uses the network stack that sets up).
 */
#pragma once

#include "brew/drivers/interfaces/clock.h"

#include <cstdint>

namespace brew::drivers {

class EspSntpClock final : public hal::WallClock {
public:
    explicit EspSntpClock(const char * server = "pool.ntp.org") : server_(server) {}

    bool begin() override;
    bool valid() override;
    int64_t unixSeconds() override;

private:
    const char * server_;
    bool started_ = false;
};

}  // namespace brew::drivers
