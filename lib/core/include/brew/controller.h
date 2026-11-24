/**
 * The fermentation controller service: owns setpoints/mode/units, reads the sensor, runs the
 * control law, drives the outputs through the guard, keeps history, persists settings, and
 * publishes State/History through a ControlPort. Runs in one task; never touches the UI.
 */
#pragma once

#include "brew/control.h"
#include "brew/ports.h"
#include "brew/drivers/interfaces/platform.h"
#include "brew/drivers/interfaces/sensor.h"

namespace brew {

class Controller {
public:
    struct Config {
        uint32_t period_ms = 200;           // sensor -> law -> outputs -> publish
        uint32_t history_period_ms = 200;   // one history sample (the mean reading) every N ms; the
                                            // graph window is History::kCapacity samples (1 h)
        float    setpoint_c[kModeCount] = { f_to_c(66.0f), f_to_c(34.0f) };   // defaults
        /* Settings the display can change (saved; these are the values until then). They're
         * applied to the law and the guard at begin(), so boards needn't keep them in step. */
        float    heat_diff_c = ChamberDefaults::kHeatDiffF * 5.0f / 9.0f;
        float    cool_diff_c = ChamberDefaults::kCoolDiffF * 5.0f / 9.0f;
        uint16_t heat_delay_s = ChamberDefaults::kRestartDelayMs / 1000u;
        uint16_t cool_delay_s = ChamberDefaults::kRestartDelayMs / 1000u;
        float    crash_default_c = f_to_c(34.0f);
        /* The schedule offered until the user edits it: 66 F for 4 days, then up to 70 F over a
         * day for a 3-day diacetyl rest. */
        uint8_t      nsteps = 2;
        ScheduleStep steps[Settings::kMaxSteps] = { { f_to_c(66.0f), 0, 96 }, { f_to_c(70.0f), 24, 72 } };
        /* The day count's clock: seconds the controller has been powered, saved every this many
         * seconds so it carries on after a power cut (an outage loses at most this much, plus
         * the time unplugged). Becomes real time once there is WiFi/NTP. */
        uint32_t clock_save_s = 600;
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
    const Settings & settings() const { return set_; }
    /** Drops saved Settings › Heat / Cool values, so Config's apply at the next begin(). */
    static void forgetTuning(hal::SettingsStore & store);

    /** The day count's clock (seconds) and when Day 1 began on it. */
    uint32_t clockS() const { return clk_s_; }
    uint32_t dayStartS() const { return day0_s_; }

private:
    void handle(const Command & c, uint32_t now_ms);
    void evaluate(uint32_t now_ms);
    void persist();
    void pumpSync();
    void advanceClock(uint32_t now_ms);
    void saveClock();
    void updateDay();
    void applyTuning();
    bool setTuning(Tuning t, int32_t v);
    void publishSettings(uint32_t now_ms);
    void startSchedule();
    void saveSchedule();
    float scheduleTarget();   /* the Ferment target the schedule wants now; updates step / override */
    bool scheduleRunning() const { return set_.profile == Profile::Schedule && set_.nsteps > 0; }

    ControlPort & port_;
    hal::TempSensor & sensor_;
    ControlLaw & law_;
    OutputGuard & guard_;
    hal::SettingsStore * store_;
    Config cfg_;

    State st_;
    Settings set_;
    uint32_t last_settings_ = 0;   // when Settings were last published (re-sent every few seconds)

    /* schedule (Settings carries its steps, the running step and the override flag) */
    uint32_t sched_start_s_ = 0;   // day-count clock when step 1 began
    float sched_from_c_ = 0.0f;    // the Ferment target then (step 1 ramps from it)
    float override_c_ = 0.0f;      // the dial's target, while set_.override_on
    History hist_;
    uint32_t last_eval_ = 0;
    uint32_t last_hist_ = 0;
    float hist_sum_ = 0.0f;     // valid readings since the last sample, averaged into it
    uint32_t hist_cnt_ = 0;
    int sync_i_ = -1;           // next history index to send in a sync, -1 = not syncing
    bool sync_reset_ = false;   // the next sync chunk is the first (reset) one
    bool started_ = false;

    /* day count */
    uint32_t clk_s_ = 0;         // seconds on the day-count clock
    uint32_t clk_ms_ = 0;        // milliseconds toward the next second
    uint32_t clk_last_ms_ = 0;   // tick time the clock last advanced to
    uint32_t clk_saved_s_ = 0;   // clk_s_ as last saved
    uint32_t day0_s_ = 0;        // clk_s_ when Day 1 began
};

}  // namespace brew
