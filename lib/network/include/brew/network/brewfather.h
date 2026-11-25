/**
 * Brewfather from the display: checks the account, posts readings to its Custom Stream, and
 * finds which batch the readings land on. Portable (std + interfaces), ticked from the UI task;
 * the HTTP client runs requests in the background, so nothing here waits on the network.
 *
 *   Account check  GET https://api.brewfather.app/v2/batches?status=Fermenting&include=devices
 *                  (Basic auth "userid:apikey"). 200: connected; 401/403: rejected. Our device in a
 *                  batch's devices.stream.items means the readings land on that batch.
 *   Logging        only for the batch imported on the dial (so readings can't land on another
 *                  one): POST https://log.brewfather.net/stream?id=<id> every 15 min (Brewfather
 *                  ignores a device posting more often), then a check to see where it landed.
 *                  Attaching the device to a batch is a manual step in Brewfather; the check
 *                  reads where it's attached, and posting stops if that's a different batch.
 *
 *   Import         the check above lists the Fermenting batches; fetchBatch() gets one whole
 *                  (GET /v2/batches/<id>) and turns its fermentation steps into a preview the
 *                  display shows, then sends to the controller.
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

/** A Fermenting batch, as listed. */
struct BfBatch {
    char id[40] = { 0 };
    char label[48] = { 0 };   // "#18 Pale Ale"
};

/** One fermentation step, converted: ramp to temp_c over ramp_h hours (0 = jump), hold hold_h. */
struct BfStep {
    char type[16] = { 0 };    // "Primary", "Conditioning", ...
    float temp_c = 0.0f;
    uint16_t ramp_h = 0, hold_h = 0;
};

/** A batch fetched for import. */
struct BfPreview {
    static constexpr int kMaxSteps = 8;      // what the controller's schedule holds
    enum class State : uint8_t { None, Loading, Ready, Failed } state = State::None;
    char id[40] = { 0 };
    char label[48] = { 0 };   // "#18 Pale Ale"
    char name[16] = { 0 };    // short, for the dial's home screen: "Pale Ale"
    int total_steps = 0;      // in Brewfather (only the first kMaxSteps are kept)
    int nsteps = 0;
    BfStep steps[kMaxSteps];
    int64_t start_unix_ms = 0;     // fermentationStartDate (0: not set)
    int64_t modified_unix_ms = 0;  // _timestamp_ms: the batch's last change
};

/** Logging, for the batch imported on the dial. */
enum class BfLog : uint8_t {
    Off,           // no batch imported: nothing is posted
    NotAttached,   // posting; our device isn't attached to a Fermenting batch yet
    Attached,      // posting, and Brewfather files it under the imported batch
    Elsewhere,     // our device is attached to a different batch: not posting
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
    /* import */
    static constexpr int kMaxBatches = 8;
    int nbatches = 0;
    BfBatch batches[kMaxBatches];   // the Fermenting batches, from the last check
    BfPreview preview;
    uint32_t rev = 0;               // bumped whenever the batches or the preview change
    /* logging */
    BfLog log = BfLog::Off;
    char logging_id[40] = { 0 };    // the imported batch...
    char logging[48] = { 0 };       // ..."#18 Pale Ale"
    char elsewhere[64] = { 0 };     // the batch our device is attached to instead (BfLog::Elsewhere)
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
    /** Fetch batch `i` of status().batches for import (status().preview shows how it goes). */
    void fetchBatch(int i, uint32_t now_ms);
    /** The preview was imported: remember the batch (and its version) to keep it in sync, and log
     *  to it from now on. */
    void imported(uint32_t now_ms);
    /** Log to batch `id` ("#18 Pale Ale"): posting starts shortly, a check finds where it lands. */
    void startLogging(const char * id, const char * label, uint32_t now_ms);
    /** The batch imported last ("" if none). */
    const char * importedId() const { return st_.logging_id; }

    /** Brewfather's names, cut for the dial: the batch's name, or the recipe's when the batch is
     *  just "Batch", without Brewfather's "(Modified) " prefix; at most n-1 bytes, whole characters. */
    static void shortName(const char * batch_name, const char * recipe_name, char * out, size_t n);

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
    enum class Pending : uint8_t { None, Check, Post, Fetch };
    bool haveAccount() const { return user_[0] && key_[0]; }
    void startCheck(uint32_t now);
    void startPost(uint32_t now);
    void finishCheck(const hal::HttpClientResult & r, uint32_t now);
    void finishPost(const hal::HttpClientResult & r, uint32_t now);
    void finishFetch(const hal::HttpClientResult & r);
    bool startGet(const char * url);
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
    bool fetch_due_ = false;
    bool reading_valid_ = false;
    float temp_c_ = 0.0f, target_c_ = 0.0f;
    char state_[12] = { 0 };
    /* request buffers (the client copies them; kept here so start() sees stable memory) */
    /* "Basic " + base64 of the longest user:key (63 + 1 + 95 bytes -> 212) fits */
    char url_[256] = { 0 }, auth_[256] = { 0 }, body_[256] = { 0 };
};

const char * accountName(BfAccount a);

}  // namespace brew::net
