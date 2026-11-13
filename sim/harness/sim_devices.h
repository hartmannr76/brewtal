/**
 * Simulated hardware for the test harness. Each class implements a brew_hal interface, so the
 * real Controller / KnobInput / UiBridge / link code runs unchanged on top of them; the web page
 * (or a scripted test) drives them instead of physical parts.
 */
#pragma once

#include "brew/fake_plant.h"
#include "brew/hal/input.h"
#include "brew/hal/io.h"
#include "brew/hal/platform.h"
#include "brew/hal/sensor.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <string>

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

private:
    FakePlant & plant_;
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

}  // namespace brew::sim
