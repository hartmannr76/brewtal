/**
 * Thermostat building blocks. Pure logic: no hardware, no RTOS, unit-tested on host.
 *
 *   TempSensor --> ControlLaw (what we want) --> OutputGuard (what is safe now) --> relays
 */
#pragma once

#include "brew/types.h"
#include "brew/drivers/interfaces/io.h"

#include <memory>
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
 *   - per output, min_off_ms: the restart delay. Once that relay has switched off it stays
 *     off at least this long. Nothing is counted from power-up: an output that hasn't run
 *     yet may start at once.
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
    uint32_t since_ms_ = 0;           // when cur_ started
    uint32_t off_ms_[3] = {0, 0, 0};  // when each Action last stopped (index = Action)
    bool ever_[3] = {false, false, false};   // Action has run since power-up (min_off, changeover)
};

/**
 * A sensible starting point for a fermentation chamber (fridge or freezer + heat wrap):
 * 0.5 F differentials (heat on at target - 0.5, runs to the target; cooling likewise) and a
 * 3 minute restart delay per relay: once heating or cooling switches off, that relay can't
 * switch back on for 3 minutes. Nothing else: no minimum on time, no delay after power-up and
 * no gap between heating and cooling. Boards use these unless they have a reason not to; the
 * web harness can compare them with the original deadband.
 */
struct ChamberDefaults {
    static constexpr float    kHeatDiffF = 0.5f;
    static constexpr float    kCoolDiffF = 0.5f;
    static constexpr uint32_t kRestartDelayMs = 3u * 60u * 1000u;   // per relay (min_off_ms)
    static constexpr uint32_t kChangeoverMs = 0;

    static OutputGuard::Config guard()
    {
        OutputGuard::Config g;
        g.heat.min_off_ms = kRestartDelayMs;
        g.cool.min_off_ms = kRestartDelayMs;
        g.changeover_ms = kChangeoverMs;
        return g;
    }
};

/**
 * Fixed-capacity history, oldest first: the graph's time window. Capacity matches brew_ui's
 * BREW_HIST_MAX (18000 samples = 1 h at the default one sample every 200 ms). When full, push()
 * drops the oldest. The buffer (72 KB) is on the heap, so on boards with PSRAM it lands there
 * instead of in internal RAM.
 */
class History {
public:
    static constexpr int kCapacity = 18000;

    void clear() { n_ = 0; }
    void push(float c);
    void assign(const float * c, int n);   // keeps the newest kCapacity
    int size() const { return n_; }
    float at(int i) const { return buf_[i]; }

private:
    std::unique_ptr<float[]> buf_{ new float[kCapacity]() };
    int n_ = 0;
};

}  // namespace brew
