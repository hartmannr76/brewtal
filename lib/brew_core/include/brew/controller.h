/**
 * The fermentation controller service: owns setpoints/mode/units, reads the sensor, runs the
 * control law, drives the outputs through the guard, keeps history, persists settings, and
 * publishes State/History through a ControlPort. Runs in one task; never touches the UI.
 */
#pragma once

#include "brew/control.h"
#include "brew/ports.h"
#include "brew/hal/platform.h"
#include "brew/hal/sensor.h"

namespace brew {

class Controller {
public:
    struct Config {
        uint32_t period_ms = 200;           // sensor -> law -> outputs -> publish
        uint32_t history_period_ms = 200;   // one history sample (the mean reading) every N ms; the
                                            // graph window is History::kCapacity samples (1 h)
        float    setpoint_c[kModeCount] = { f_to_c(66.0f), f_to_c(34.0f) };   // defaults
        const char * batch = "Pale ale";
        uint16_t day = 1;
    };

    Controller(ControlPort & port, hal::TempSensor & sensor, ControlLaw & law, OutputGuard & guard,
               hal::SettingsStore * store, const Config & cfg);

    /** Loads persisted settings and publishes the (RAM) history, so a display that outlives a
     *  controller reboot drops its old graph. Call once before tick(). */
    void begin(uint32_t now_ms);

    /** Call often (every ~10-50 ms): handles commands immediately, evaluates every period_ms. */
    void tick(uint32_t now_ms);

    /** Re-read the sensor and re-evaluate now (e.g. a scripted temperature change in the sim). */
    void evaluateNow(uint32_t now_ms);

    /** Direct access for demo/sim seeding. Call syncHistory() after editing. */
    History & history() { return hist_; }
    /** (Re)starts sending the whole history. A full window is ~150 chunks, more than the
     *  port can queue, so tick() sends it a chunk at a time as the port has room. */
    void syncHistory();
    bool syncing() const { return sync_i_ >= 0; }

    const State & state() const { return st_; }
    void setBatch(const char * name, uint16_t day);

private:
    void handle(const Command & c, uint32_t now_ms);
    void evaluate(uint32_t now_ms);
    void persist();
    void pumpSync();

    ControlPort & port_;
    hal::TempSensor & sensor_;
    ControlLaw & law_;
    OutputGuard & guard_;
    hal::SettingsStore * store_;
    Config cfg_;

    State st_;
    History hist_;
    uint32_t last_eval_ = 0;
    uint32_t last_hist_ = 0;
    float hist_sum_ = 0.0f;     // valid readings since the last sample, averaged into it
    uint32_t hist_cnt_ = 0;
    int sync_i_ = -1;           // next history index to send in a sync, -1 = not syncing
    bool sync_reset_ = false;   // the next sync chunk is the first (reset) one
    bool started_ = false;
};

}  // namespace brew
