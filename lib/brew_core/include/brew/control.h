/**
 * Thermostat building blocks. Pure logic: no hardware, no RTOS, unit-tested on host.
 *
 *   TempSensor --> ControlLaw (what we want) --> OutputGuard (what is safe now) --> relays
 */
#pragma once

#include "brew/types.h"
#include "brew/hal/io.h"

#include <cstdint>

namespace brew {

/** Decides heating / cooling / idle. Swap implementations per board or per preference. */
class ControlLaw {
public:
    virtual ~ControlLaw() = default;
    virtual Action step(float temp_c, float setpoint_c, uint32_t now_ms) = 0;
    virtual void   reset() {}
};

/**
 * Stateless deadband: cool above setpoint + cool_band, heat below setpoint - heat_band, else
 * idle, switching off again as soon as the reading is back inside. The original BrewDial
 * behaviour (0.3 F both ways); it short-cycles, so pair it with OutputGuard min times.
 */
class DeadbandLaw final : public ControlLaw {
public:
    explicit DeadbandLaw(float band_c) : DeadbandLaw(band_c, band_c) {}
    DeadbandLaw(float heat_band_c, float cool_band_c) : heat_(heat_band_c), cool_(cool_band_c) {}
    Action step(float t, float sp, uint32_t) override
    {
        if(t > sp + cool_) return Action::Cooling;
        if(t < sp - heat_) return Action::Heating;
        return Action::Idle;
    }

private:
    float heat_, cool_;
};

/**
 * Hysteresis with separate differentials, like an Inkbird ITC-308 (HD / CD): heating comes on
 * at setpoint - heat_diff and runs until the setpoint is reached; cooling comes on at
 * setpoint + cool_diff and runs until the setpoint is reached. Fewer, longer cycles.
 */
class HysteresisLaw final : public ControlLaw {
public:
    explicit HysteresisLaw(float diff_c) : HysteresisLaw(diff_c, diff_c) {}
    HysteresisLaw(float heat_diff_c, float cool_diff_c) : heat_(heat_diff_c), cool_(cool_diff_c) {}
    Action step(float t, float sp, uint32_t) override
    {
        switch(cur_) {
            case Action::Cooling: if(t <= sp) cur_ = Action::Idle; break;
            case Action::Heating: if(t >= sp) cur_ = Action::Idle; break;
            case Action::Idle:    break;
        }
        if(cur_ == Action::Idle) {
            if(t > sp + cool_) cur_ = Action::Cooling;
            else if(t < sp - heat_) cur_ = Action::Heating;
        }
        return cur_;
    }
    void reset() override { cur_ = Action::Idle; }

private:
    float heat_, cool_;
    Action cur_ = Action::Idle;
};

/**
 * Turns the wanted Action into relay states, enforcing:
 *   - never heat and cool at once (interlock)
 *   - per output, min_on_ms:  once on, stay on at least this long
 *   - per output, min_off_ms: once off, stay off at least this long. Also counted from
 *     power-up (the first apply()), so a power blip can't restart a compressor at once:
 *     the Inkbird "compressor delay".
 *   - changeover_ms: idle time required between heating and cooling
 * All default to 0 (original behaviour). Returns what the outputs are actually doing.
 */
class OutputGuard {
public:
    struct Timing {
        uint32_t min_on_ms = 0;
        uint32_t min_off_ms = 0;
    };
    struct Config {
        Timing heat;
        Timing cool;
        uint32_t changeover_ms = 0;
    };

    OutputGuard(hal::DigitalOut * heat, hal::DigitalOut * cool, Config cfg);
    Action apply(Action want, uint32_t now_ms);
    Action current() const { return cur_; }
    /** ms until `a` may switch on (0 = now, or `a` is Idle). For status displays. */
    uint32_t blockedFor(Action a, uint32_t now_ms) const;

private:
    void drive();
    const Timing & timing(Action a) const { return a == Action::Heating ? cfg_.heat : cfg_.cool; }

    hal::DigitalOut * heat_;
    hal::DigitalOut * cool_;
    Config cfg_;
    Action cur_ = Action::Idle;
    bool started_ = false;
    uint32_t since_ms_ = 0;           // when cur_ started
    uint32_t off_ms_[3] = {0, 0, 0};  // when each Action last stopped (index = Action); power-up
    bool ever_[3] = {false, false, false};   // Action has run since power-up (for changeover)
};

/**
 * A sensible starting point for a fermentation chamber (fridge or freezer + heat wrap), in the
 * spirit of an Inkbird ITC-308: 0.5 F differentials, a 5 minute compressor delay, and a
 * 10 minute idle gap before switching between heating and cooling. Boards use these unless
 * they have a reason not to; the web harness can compare them with the original deadband.
 */
struct ChamberDefaults {
    static constexpr float    kHeatDiffF = 0.5f;
    static constexpr float    kCoolDiffF = 0.5f;
    static constexpr uint32_t kCoolMinOffMs = 5u * 60u * 1000u;
    static constexpr uint32_t kChangeoverMs = 10u * 60u * 1000u;

    static OutputGuard::Config guard()
    {
        OutputGuard::Config g;
        g.cool.min_off_ms = kCoolMinOffMs;
        g.changeover_ms = kChangeoverMs;
        return g;
    }
};

/**
 * Fixed-capacity history, oldest first: the graph's time window. Capacity matches brew_ui's
 * HIST_MAX (120 samples = 2 h at the default one sample per minute).
 */
class History {
public:
    static constexpr int kCapacity = 120;

    void clear() { n_ = 0; }
    void push(float c);
    void assign(const float * c, int n);   // keeps the newest kCapacity
    int size() const { return n_; }
    float at(int i) const { return buf_[i]; }

private:
    float buf_[kCapacity] = {};
    int n_ = 0;
};

}  // namespace brew
