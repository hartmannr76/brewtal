/**
 * Brewfather from the display: checks the account, posts readings to its Custom Stream, and
 * finds which batch the readings land on. Portable (std + interfaces), ticked from the UI task;
 * the HTTP client runs requests in the background, so nothing here waits on the network.
 *
 *   Account check  GET https://api.brewfather.app/v2/batches?status=Fermenting&include=devices
 *                  (Basic auth "userid:apikey"). 200: connected; 401/403: rejected. Our device in a
 *                  batch's devices.stream.items means the readings land on that batch.
 *   Logging        POST https://log.brewfather.net/stream?id=<id> every 15 min (Brewfather
 *                  ignores a device posting more often), then a check to see it land.
 *
 * Credentials are typed key/value settings (bf_user, bf_key, bf_stream) and are never shown:
 * status() says only whether each is set.
 */
#pragma once

#include "brew/drivers/interfaces/http.h"
#include "brew/drivers/interfaces/platform.h"

#include <cstddef>
#include <cstdint>

namespace brew::net {

enum class BfAccount : uint8_t {
    NotSetUp,      // no user id / API key saved
    Checking,      // the first check is running
    Connected,     // the API accepted the key
    Rejected,      // 401/403: wrong user id or key (or no API access)
    Unreachable,   // no answer, or a server error: tried again later
};

struct BrewfatherStatus {
    BfAccount account = BfAccount::NotSetUp;
    int http = 0;                  // the API's last status (negative: no response)
    bool user_set = false, key_set = false, stream_set = false;
    char device[24] = { 0 };       // our Custom Stream device name: "BrewDialCB50"
    /* where Brewfather files our readings (a Fermenting batch with our device attached) */
    bool attached = false;
    char batch[64] = { 0 };        // "#18 Pale Ale"
    int64_t last_log_unix_ms = 0;  // when Brewfather last recorded a reading from us (0: never)
    /* our side of logging */
    uint32_t last_post_ms = 0;     // when we last posted (0: not yet)
    int last_post_http = 0;
};

class Brewfather {
public:
    struct Config {
        uint32_t post_every_ms = 15u * 60u * 1000u + 15000u;   // Brewfather: at most one per 15 min
        uint32_t first_post_ms = 20000;                        // after coming online / new credentials
        uint32_t check_every_ms = 30u * 60u * 1000u;
        uint32_t check_after_post_ms = 90000;                  // did the post land on a batch?
        uint32_t retry_ms = 5u * 60u * 1000u;                  // after Unreachable
        const char * api = "https://api.brewfather.app";
        const char * stream = "https://log.brewfather.net";
    };

    Brewfather(hal::HttpClient & http, hal::SettingsStore * store) : Brewfather(http, store, Config{}) {}
    Brewfather(hal::HttpClient & http, hal::SettingsStore * store, const Config & cfg);

    /** Loads saved credentials. `device` is our Custom Stream name (letters and digits only:
     *  Brewfather drops anything else). */
    void begin(const char * device, uint32_t now_ms);
    /** From the phone page. An empty or null field keeps what's saved; a check follows at once. */
    void setCredentials(const char * user, const char * key, const char * stream, uint32_t now_ms);
    /** Forgets all three and stops. */
    void forget();
    /** Check the account and attachment at the next tick. */
    void checkNow(uint32_t now_ms);

    /** The latest reading, posted at the next 15-minute slot. `state`: "heating", "cooling", "idle". */
    void setReading(bool valid, float temp_c, float target_c, const char * state);

    /** `online`: the home WiFi is up. `unix_ms`: wall-clock time, 0 if not known yet. */
    void tick(uint32_t now_ms, bool online, int64_t unix_ms);

    const BrewfatherStatus & status() const { return st_; }
    /** Seconds since Brewfather last recorded a reading from us, -1 if unknown. */
    int64_t lastLogAgeS(int64_t unix_ms) const;
    /** The same, at the wall-clock time of the last tick. */
    int64_t lastLogAgeS() const { return lastLogAgeS(unix_ms_); }

    /** Base64 (for the Basic auth header). Writes a terminated string; false if `n` is too small. */
    static bool base64(const char * in, size_t len, char * out, size_t n);

private:
    enum class Pending : uint8_t { None, Check, Post };
    bool haveAccount() const { return user_[0] && key_[0]; }
    void startCheck(uint32_t now);
    void startPost(uint32_t now);
    void finishCheck(const hal::HttpClientResult & r, uint32_t now);
    void finishPost(const hal::HttpClientResult & r, uint32_t now);
    void save();

    hal::HttpClient & http_;
    hal::SettingsStore * store_;
    Config cfg_;
    BrewfatherStatus st_;
    char user_[64] = { 0 }, key_[96] = { 0 }, stream_[64] = { 0 };
    char key_upper_[24] = { 0 };   // our device as Brewfather keys it: "BREWDIALCB50"
    Pending pending_ = Pending::None;
    int64_t unix_ms_ = 0;
    uint32_t check_at_ = 0, post_at_ = 0;
    bool check_due_ = false, post_due_ = false;
    bool reading_valid_ = false;
    float temp_c_ = 0.0f, target_c_ = 0.0f;
    char state_[12] = { 0 };
    /* request buffers (the client copies them; kept here so start() sees stable memory) */
    char url_[256] = { 0 }, auth_[192] = { 0 }, body_[256] = { 0 };
};

const char * accountName(BfAccount a);

}  // namespace brew::net
