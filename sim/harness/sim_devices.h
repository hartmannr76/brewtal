/**
 * Simulated hardware for the test harness. Each class implements a driver interface (lib/drivers/interfaces), so the
 * real Controller / KnobInput / UiBridge / link code runs unchanged on top of them; the web page
 * (or a scripted test) drives them instead of physical parts.
 */
#pragma once

#include "brew/fake_plant.h"
#include "brew/drivers/interfaces/clock.h"
#include "brew/drivers/interfaces/http.h"
#include "brew/drivers/interfaces/input.h"
#include "brew/drivers/interfaces/io.h"
#include "brew/drivers/interfaces/platform.h"
#include "brew/drivers/interfaces/sensor.h"
#include "brew/drivers/interfaces/wifi.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <cctype>
#include <string>
#include <vector>

namespace brew::sim {

/** Quadrature encoder whose count the page sets directly. */
class SimEncoder final : public hal::Encoder {
public:
    bool begin() override { return true; }
    int32_t count() override { return count_; }
    void add(int32_t counts) { count_ += counts; }

private:
    int32_t count_ = 0;
};

/** Push switch level (active low, like the CrowPanel's PCF8574 input). */
class SimButton final : public hal::DigitalIn {
public:
    bool read() override { return !down_; }
    void set(bool down) { down_ = down; }
    bool down() const { return down_; }

private:
    bool down_ = false;
};

/** Relay / LED output: remembers its level and how it has been used. */
class SimRelay final : public hal::DigitalOut {
public:
    explicit SimRelay(const uint32_t & clock) : clock_(clock) {}

    void write(bool high) override
    {
        if(high == on_) return;
        const uint32_t now = clock_;
        if(on_) on_total_ms_ += now - since_;
        on_ = high;
        since_ = now;
        switches_++;
    }

    bool on() const { return on_; }
    uint32_t switches() const { return switches_; }
    /** Total on time including the current run. */
    uint64_t onMs() const { return on_total_ms_ + (on_ ? clock_ - since_ : 0); }
    /** How long it has been in its current state. */
    uint32_t inStateMs() const { return clock_ - since_; }

private:
    const uint32_t & clock_;
    bool on_ = false;
    uint32_t since_ = 0;
    uint32_t switches_ = 0;
    uint64_t on_total_ms_ = 0;
};

/**
 * The probe the controller reads. The beer itself is a FakePlant (reacts to the relays when
 * `physics` is on); faults are applied between the beer and the reading.
 */
class SimProbe final : public hal::TempSensor {
public:
    enum class Fault : uint8_t { None = 0, Disconnected = 1, Stuck = 2, Noisy = 3 };

    explicit SimProbe(FakePlant & plant) : plant_(plant) {}

    bool begin() override { return true; }

    void update(uint32_t now) override
    {
        if(physics) plant_.update(now);
        else {
            plant_.update(now);           // keep its clock moving
            plant_.set(hold_c_);          // ...but the temperature stays where the page put it
        }
        /* like a DS18B20 at 12 bits: a new reading every period_ms, held in between */
        if(have_ && now - read_ms_ < period_ms) return;
        have_ = true;
        read_ms_ = now;
        hal::TempReading r = plant_.latest();
        r.celsius += offset_c;
        switch(fault) {
            case Fault::None: break;
            case Fault::Disconnected: r.valid = false; break;
            case Fault::Stuck: r.celsius = stuck_c_; break;
            case Fault::Noisy: r.celsius += noise_(rng_) * noise_c; break;
        }
        if(fault != Fault::Stuck) stuck_c_ = r.celsius;
        r.at_ms = now;
        last_ = r;
    }

    hal::TempReading latest() const override { return last_; }
    const char * name() const override { return "sim-probe"; }
    /* The bench plays a real probe; set this to see the dial as a demo build shows it. */
    bool demo = false;
    bool simulated() const override { return demo; }

    /** Put the beer at `c` (and keep it there while physics is off). */
    void setBeer(float c)
    {
        hold_c_ = c;
        plant_.set(c);
    }
    void setPhysics(bool on)
    {
        physics = on;
        hold_c_ = plant_.actual();
    }

    bool physics = true;
    Fault fault = Fault::None;
    float noise_c = 0.3f;    // 1 sigma
    float offset_c = 0.0f;   // calibration error
    uint32_t period_ms = 800;   // how often a new reading lands (the real probe: ~800 ms)

private:
    FakePlant & plant_;
    bool have_ = false;
    uint32_t read_ms_ = 0;
    hal::TempReading last_;
    float hold_c_ = 20.0f;
    float stuck_c_ = 0.0f;
    std::mt19937 rng_{ 12345 };
    std::normal_distribution<float> noise_{ 0.0f, 1.0f };
};

/** In-memory settings: survive a simulated controller reboot, like NVS survives a real one. */
class MemSettings final : public hal::SettingsStore {
public:
    bool getI32(const char * key, int32_t & out) override
    {
        auto it = i32_.find(key);
        if(it == i32_.end()) return false;
        out = it->second;
        return true;
    }
    bool setI32(const char * key, int32_t v) override
    {
        i32_[key] = v;
        return true;
    }
    bool getStr(const char * key, char * out, size_t max) override
    {
        auto it = str_.find(key);
        if(it == str_.end() || max == 0) return false;
        std::snprintf(out, max, "%s", it->second.c_str());
        return true;
    }
    bool setStr(const char * key, const char * v) override
    {
        str_[key] = v ? v : "";
        return true;
    }
    bool commit() override
    {
        commits++;
        return true;
    }
    void clear()
    {
        i32_.clear();
        str_.clear();
    }

    uint32_t commits = 0;

private:
    std::map<std::string, int32_t> i32_;
    std::map<std::string, std::string> str_;
};

/**
 * A cable between two boards: two byte queues with fault injection. Each end is a
 * hal::ByteStream, so StreamPacketLink + the remote proxies run exactly as on hardware.
 */
class SimWire {
public:
    class End final : public hal::ByteStream {
    public:
        End(SimWire & w, int side) : w_(w), side_(side) {}
        bool begin() override { return true; }
        size_t write(const uint8_t * d, size_t n) override { return w_.put(side_, d, n); }
        size_t read(uint8_t * buf, size_t max) override { return w_.get(side_, buf, max); }

    private:
        SimWire & w_;
        int side_;
    };

    End & a() { return a_; }
    End & b() { return b_; }

    void clear()
    {
        q_[0].clear();
        q_[1].clear();
    }

    bool unplugged = false;
    float drop_rate = 0.0f;      // probability each byte is lost
    float corrupt_rate = 0.0f;   // probability each byte gets a flipped bit
    uint64_t bytes[2] = { 0, 0 };          // delivered, per direction (0: a->b, 1: b->a)
    uint64_t dropped = 0, corrupted = 0;

private:
    static constexpr size_t kCap = 64 * 1024;   // a UART FIFO would overflow long before this

    size_t put(int side, const uint8_t * d, size_t n)
    {
        std::deque<uint8_t> & q = q_[side];
        for(size_t i = 0; i < n; i++) {
            if(unplugged || q.size() >= kCap || (drop_rate > 0 && u_(rng_) < drop_rate)) {
                dropped++;
                continue;
            }
            uint8_t c = d[i];
            if(corrupt_rate > 0 && u_(rng_) < corrupt_rate) {
                c ^= static_cast<uint8_t>(1u << (rng_() & 7));
                corrupted++;
            }
            q.push_back(c);
            bytes[side]++;
        }
        return n;   // a UART accepts the bytes whether or not anyone is listening
    }

    size_t get(int side, uint8_t * buf, size_t max)
    {
        std::deque<uint8_t> & q = q_[1 - side];   // read what the other end wrote
        size_t n = 0;
        while(n < max && !q.empty()) {
            buf[n++] = q.front();
            q.pop_front();
        }
        return n;
    }

    End a_{ *this, 0 }, b_{ *this, 1 };
    std::deque<uint8_t> q_[2];
    std::mt19937 rng_{ 777 };
    std::uniform_real_distribution<float> u_{ 0.0f, 1.0f };
};

/**
 * A WiFi radio in a made-up neighbourhood. Joining takes connect_ms; scanning takes scan_ms.
 * Any password works except "wrong" (and secured networks want at least 8 characters, like
 * WPA2). Turning the router off makes every network vanish and drops the link. When the setup
 * network closes, the phone on it falls off, as a real one would.
 */
class SimWifi;

/**
 * Internet time for the harness: a fixed date (2026-11-20 00:00 UTC) plus simulated time, so
 * fast-forwarding moves the date too. Like SNTP it becomes valid once the WiFi is up, and stays
 * valid if it drops.
 */
class SimWallClock final : public hal::WallClock {
public:
    static constexpr int64_t kEpoch = 1795132800;
    SimWallClock(const uint32_t & sim_ms, SimWifi & wifi) : ms_(sim_ms), wifi_(wifi) {}
    bool begin() override { return true; }
    bool valid() override;
    int64_t unixSeconds() override { return kEpoch + ms_ / 1000; }

private:
    const uint32_t & ms_;
    SimWifi & wifi_;
    bool set_ = false;
};

class SimWifi final : public hal::Wifi {
public:
    struct Net {
        std::string ssid;
        int rssi;
        bool secured;
    };

    explicit SimWifi(const uint32_t & now) : now_(now) {}

    std::vector<Net> networks = { { "Hartmann-5G", -48, true }, { "Hartmann-IoT", -61, true },
                                  { "NETGEAR27", -72, true }, { "xfinitywifi", -83, false } };
    bool router_up = true;
    bool phone = false;          // a phone has joined the setup network
    uint32_t connect_ms = 2500;
    uint32_t scan_ms = 1500;

    bool apOn() const { return ap_; }
    const std::string & apSsid() const { return ap_ssid_; }
    const std::string & joined() const { return joined_; }

    bool begin() override { return true; }
    uint32_t deviceId() override { return 0x00A14F2Au; }
    uint32_t random() override { return rng_(); }

    bool startAccessPoint(const char * ssid, const char * password) override
    {
        ap_ = true;
        ap_ssid_ = ssid;
        ap_pw_ = password;
        return true;
    }
    void stopAccessPoint() override
    {
        ap_ = false;
        phone = false;
    }
    int accessPointClients() override { return ap_ && phone ? 1 : 0; }

    bool startScan() override
    {
        scanning_ = true;
        scan_done_ = now_ + scan_ms;
        return true;
    }
    int scanResults(hal::WifiNetwork * out, int max) override
    {
        if(scanning_ && static_cast<int32_t>(now_ - scan_done_) < 0) return -1;
        scanning_ = false;
        if(!router_up) return 0;
        int n = 0;
        for(const Net & w : networks) {
            if(n >= max) break;
            std::snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", w.ssid.c_str());
            out[n].rssi = static_cast<int8_t>(w.rssi);
            out[n].secured = w.secured;
            n++;
        }
        return n;
    }

    bool connect(const char * ssid, const char * password) override
    {
        try_ssid_ = ssid ? ssid : "";
        try_pw_ = password ? password : "";
        link_ = hal::WifiLink::Connecting;
        result_at_ = now_ + connect_ms;
        joined_.clear();
        return true;
    }
    void disconnect() override
    {
        link_ = hal::WifiLink::Down;
        joined_.clear();
    }
    hal::WifiLink link() override
    {
        if(link_ == hal::WifiLink::Connecting && static_cast<int32_t>(now_ - result_at_) >= 0) {
            const Net * w = nullptr;
            for(const Net & n : networks)
                if(n.ssid == try_ssid_) w = &n;
            if(!router_up || !w) link_ = hal::WifiLink::NotFound;
            else if(w->secured && (try_pw_ == "wrong" || try_pw_.size() < 8)) link_ = hal::WifiLink::WrongPassword;
            else {
                link_ = hal::WifiLink::Up;
                joined_ = try_ssid_;
            }
        }
        if(link_ == hal::WifiLink::Up && !router_up) {
            link_ = hal::WifiLink::Down;
            joined_.clear();
        }
        return link_;
    }
    uint32_t stationAddress() override { return link() == hal::WifiLink::Up ? 0xC0A8012Au : 0u; }   // 192.168.1.42

private:
    const uint32_t & now_;
    std::mt19937 rng_{ 2026 };
    bool ap_ = false;
    std::string ap_ssid_, ap_pw_;
    bool scanning_ = false;
    uint32_t scan_done_ = 0;
    hal::WifiLink link_ = hal::WifiLink::Down;
    std::string try_ssid_, try_pw_, joined_;
    uint32_t result_at_ = 0;
};

/**
 * Brewfather for the harness, behind the dial's HTTP client: answers after a short delay (the
 * device runs requests in the background too), and only while the simulated WiFi is up.
 *   GET  api.brewfather.app/v2/batches...   one Fermenting batch, "#18 Pale Ale"; the dial's stream
 *                                           device appears in it once it has posted (as if the
 *                                           user attached it) while `attached` is on.
 *                                           API key "wrong": 401.
 *   GET  .../v2/batches/sim18               that batch whole: Primary 66 F 7 days, then up to 53 F
 *                                           over a day, held 14 days; fermentation started 3 days ago.
 *   POST log.brewfather.net/stream          200, counted in `posts`, body kept in `last_post`.
 */
class SimHttpClient final : public hal::HttpClient {
public:
    SimHttpClient(const uint32_t & now, SimWifi & wifi, hal::WallClock & clock) : now_(now), wifi_(wifi), clock_(clock) {}

    bool attached = true;
    int posts = 0;
    std::string last_post;
    uint32_t delay_ms = 600;

    bool begin() override { return true; }
    bool start(const hal::HttpClientRequest & r) override
    {
        if(busy_) return false;
        busy_ = true;
        done_at_ = now_ + delay_ms;
        method_ = r.method ? r.method : "GET";
        url_ = r.url ? r.url : "";
        auth_ = r.authorization ? r.authorization : "";
        body_.assign(r.body ? r.body : "", r.body ? r.body_len : 0);
        return true;
    }
    bool poll(hal::HttpClientResult & out) override
    {
        if(!busy_ || static_cast<int32_t>(now_ - done_at_) < 0) return false;
        busy_ = false;
        answer();
        out.status = status_;
        out.body = resp_.c_str();
        out.body_len = resp_.size();
        return true;
    }

private:
    static std::string unbase64(const std::string & s)
    {
        std::string out;
        int val = 0, bits = -8;
        for(char c : s) {
            const char * k = std::strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", c);
            if(!k || !c) break;
            val = (val << 6) + static_cast<int>(k - "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
            bits += 6;
            if(bits >= 0) {
                out.push_back(static_cast<char>((val >> bits) & 0xFF));
                bits -= 8;
            }
        }
        return out;
    }
    void answer()
    {
        resp_.clear();
        if(wifi_.link() != hal::WifiLink::Up) {
            status_ = -1;
            return;
        }
        if(url_.find("log.brewfather.net/stream") != std::string::npos && method_ == "POST") {
            posts++;
            last_post = body_;
            const size_t a = body_.find("\"name\":\"");
            if(a != std::string::npos) {
                const size_t b = body_.find('"', a + 8);
                device_ = body_.substr(a + 8, b - a - 8);
            }
            last_log_ms_ = clock_.valid() ? clock_.unixSeconds() * 1000 : 0;
            status_ = 200;
            resp_ = "{ \"result\": \"OK\" }";
            return;
        }
        if(url_.find("api.brewfather.app/v2/batches/sim18") != std::string::npos) {
            const int64_t now_ms = clock_.valid() ? clock_.unixSeconds() * 1000 : 1795132800000LL;
            const int64_t start = now_ms - 3LL * 86400000LL;
            resp_ = "{\"_id\":\"sim18\",\"name\":\"Batch\",\"batchNo\":18,\"status\":\"Fermenting\","
                    "\"fermentationStartDate\":" + std::to_string(start) + ",\"_timestamp_ms\":" + std::to_string(start) + ","
                    "\"recipe\":{\"name\":\"(Modified) Pale Ale\",\"fermentation\":{\"name\":\"Ale\",\"steps\":["
                    "{\"type\":\"Primary\",\"stepTemp\":18.888889,\"stepTime\":7,\"ramp\":null},"
                    "{\"type\":\"Conditioning\",\"stepTemp\":11.666667,\"stepTime\":14,\"ramp\":1}]}}}";
            status_ = 200;
            return;
        }
        if(url_.find("api.brewfather.app/v2/batches") != std::string::npos) {
            const std::string cred = auth_.rfind("Basic ", 0) == 0 ? unbase64(auth_.substr(6)) : std::string();
            const size_t colon = cred.find(':');
            if(colon == std::string::npos || cred.substr(colon + 1) == "wrong") {
                status_ = 401;
                return;
            }
            std::string items;
            if(attached && !device_.empty()) {
                std::string key;
                for(char c : device_) key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                items = "{\"name\":\"" + device_ + "\",\"key\":\"" + key + "\",\"type\":\"stream\",\"lastLog\":" +
                        std::to_string(last_log_ms_) + "}";
            }
            resp_ = "[{\"_id\":\"sim18\",\"batchNo\":18,\"name\":\"Batch\",\"recipe\":{\"name\":\"Pale Ale\"},"
                    "\"status\":\"Fermenting\",\"devices\":{\"stream\":{\"enabled\":true,\"items\":[" + items + "]}}}]";
            status_ = 200;
            return;
        }
        status_ = 404;
    }

    const uint32_t & now_;
    SimWifi & wifi_;
    hal::WallClock & clock_;
    bool busy_ = false;
    uint32_t done_at_ = 0;
    std::string method_, url_, auth_, body_, resp_, device_;
    int status_ = 0;
    int64_t last_log_ms_ = 0;
};

inline bool SimWallClock::valid()
{
    if(wifi_.link() == hal::WifiLink::Up) set_ = true;
    return set_;
}

}  // namespace brew::sim
