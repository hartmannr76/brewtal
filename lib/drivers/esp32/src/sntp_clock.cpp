#include "brew/drivers/esp32/sntp_clock.h"
#include "brew/drivers/esp32/log.h"

#include "esp_log.h"
#include "esp_sntp.h"

#include <ctime>
#include <sys/time.h>

namespace brew::drivers {

namespace {
const char * TAG = "brew.time";
constexpr int64_t kSane = 1735689600;   // 2025-01-01: anything earlier means "not set yet"

void synced(struct timeval * tv)
{
    BREW_LOGI(TAG, "clock set from the internet (%lld)", static_cast<long long>(tv->tv_sec));
}
}  // namespace

bool EspSntpClock::begin()
{
    if(started_) return true;
    if(esp_sntp_enabled()) esp_sntp_stop();   // someone else started it: take it over
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, server_);
    sntp_set_time_sync_notification_cb(synced);
    esp_sntp_init();
    started_ = true;
    return true;
}

bool EspSntpClock::valid() { return unixSeconds() >= kSane; }

int64_t EspSntpClock::unixSeconds()
{
    struct timeval tv = {};
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec);
}

}  // namespace brew::drivers
