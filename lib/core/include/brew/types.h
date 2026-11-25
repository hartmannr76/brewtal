/**
 * Domain types shared by the UI side and the controller side.
 *
 * These are plain C++ structs for app code. They are NEVER sent over a wire or written to
 * storage as raw bytes: only brew/codec.h turns them into bytes (see CLAUDE.md, "Wire format
 * rules"). Temperatures are degrees Celsius everywhere in core; the UI bridge converts to
 * the brew_ui API's Fahrenheit.
 */
#pragma once

#include <cstdint>

namespace brew {

enum class Mode : uint8_t { Ferment = 0, Crash = 1 };
enum class Action : uint8_t { Idle = 0, Cooling = 1, Heating = 2 };

constexpr int kModeCount = 2;

inline float f_to_c(float f) { return (f - 32.0f) * 5.0f / 9.0f; }
inline float c_to_f(float c) { return c * 9.0f / 5.0f + 32.0f; }

/** Everything the UI shows, published by the controller. Latest value wins. */
struct State {
    static constexpr int kBatchLen = 16;   // including the terminator

    float    temp_c = 0.0f;
    bool     temp_valid = false;
    float    setpoint_c[kModeCount] = { 0.0f, 0.0f };
    Mode     mode = Mode::Ferment;
    bool     paused = false;
    Action   action = Action::Idle;        // what the outputs are actually doing
    bool     celsius = false;              // display units
    char     batch[kBatchLen] = { 0 };
    uint16_t day = 0;
    Action   wanted = Action::Idle;        // what the control law asks for; differs from `action`
                                           // while OutputGuard holds it off (restart delay, ...)
    uint16_t wait_s = 0;                   // how long until `wanted` may start (0 = not waiting)
};

/**
 * Settings the user changes from the display (Settings › Heat / Cool), published by the
 * controller on change, on a sync request and every few seconds. Kept out of State, which goes
 * out five times a second: these rarely change, and a short State survives a noisy link better.
 */
/** How the Ferment target is set: by hand from the dial, or by a schedule of steps. */
enum class Profile : uint8_t { Constant = 0, Schedule = 1 };
constexpr int kProfileCount = 2;

/** One schedule step: ramp to temp_c over ramp_h hours (0 = jump), then hold it hold_h hours. */
struct ScheduleStep {
    float    temp_c = 0.0f;
    uint16_t ramp_h = 0;
    uint16_t hold_h = 24;
};

struct Settings {
    static constexpr int kMaxSteps = 8;
    static constexpr uint8_t kDone = 0xFF;   // `step` once the last step has finished

    float    heat_diff_c = 0.0f;           // heating starts this far below the setpoint
    float    cool_diff_c = 0.0f;           // cooling starts this far above it
    uint16_t heat_delay_s = 0;             // restart delays
    uint16_t cool_delay_s = 0;
    float    crash_default_c = 0.0f;       // where Cold crash starts

    Profile  profile = Profile::Constant;
    uint8_t  step = 0;                     // the running step (Schedule), or kDone
    bool     override_on = false;          // the dial set the target until the next step
    bool     simulated = false;            // the temperature comes from a simulated fermenter (demo)
    uint8_t  nsteps = 0;
    ScheduleStep steps[kMaxSteps];
};

/** What Command::Kind::SetTuning changes. Temperatures and differences in centi-C, delays in s. */
enum class Tuning : uint8_t { HeatDiff = 0, CoolDiff = 1, HeatDelay = 2, CoolDelay = 3, CrashDefault = 4 };
constexpr int kTuningCount = 5;

/** User intent, sent by the UI. Idempotent: applying one twice is harmless. */
struct Command {
    enum class Kind : uint8_t {
        SetSetpoint = 1,   // mode, setpoint_c
        SetMode = 2,       // mode, paused
        SetUnits = 3,      // celsius
        RequestSync = 4,   // controller re-sends State + full history
        StartDayCount = 5, // day count starts over at Day 1 now ("day 1 begins now": repeating it
                           // moments later changes nothing visible)
        SetTuning = 6,     // tuning, value: set one Settings value
        SetProfile = 7,    // value: Profile. Picking Schedule (from Constant) starts it at step 1
        SetStep = 8,       // step, setpoint_c, ramp_h, hold_h: set schedule step `step`
        SetStepCount = 9,  // value: how many schedule steps there are
        SetClock = 10,     // value: the time now, Unix seconds (as uint32), from the internet.
                           // The day count and schedule run on it from then on.
        SetBatch = 11,     // value: when fermentation started, Unix seconds (as uint32); name: the
                           // batch. Runs the schedule (already sent with SetStep / SetStepCount) as
                           // if started then, and Day 1 is that day: an imported Brewfather batch.
    };

    Kind  kind = Kind::RequestSync;
    Mode  mode = Mode::Ferment;
    float setpoint_c = 0.0f;
    bool  paused = false;
    bool  celsius = false;
    Tuning tuning = Tuning::HeatDiff;
    int32_t value = 0;
    uint8_t step = 0;
    uint16_t ramp_h = 0, hold_h = 0;
    char name[State::kBatchLen] = { 0 };   // SetBatch

    static Command setSetpoint(Mode m, float c) { Command x; x.kind = Kind::SetSetpoint; x.mode = m; x.setpoint_c = c; return x; }
    static Command setMode(Mode m, bool p)      { Command x; x.kind = Kind::SetMode; x.mode = m; x.paused = p; return x; }
    static Command setUnits(bool c)             { Command x; x.kind = Kind::SetUnits; x.celsius = c; return x; }
    static Command requestSync()                { return Command{}; }
    static Command startDayCount()              { Command x; x.kind = Kind::StartDayCount; return x; }
    static Command setTuning(Tuning t, int32_t v) { Command x; x.kind = Kind::SetTuning; x.tuning = t; x.value = v; return x; }
    static Command setProfile(Profile p)          { Command x; x.kind = Kind::SetProfile; x.value = static_cast<int32_t>(p); return x; }
    static Command setStep(uint8_t i, float temp_c, uint16_t ramp_h, uint16_t hold_h)
    {
        Command x;
        x.kind = Kind::SetStep;
        x.step = i;
        x.setpoint_c = temp_c;
        x.ramp_h = ramp_h;
        x.hold_h = hold_h;
        return x;
    }
    static Command setStepCount(uint8_t n)        { Command x; x.kind = Kind::SetStepCount; x.value = n; return x; }
    static Command setClock(uint32_t unix_s)      { Command x; x.kind = Kind::SetClock; x.value = static_cast<int32_t>(unix_s); return x; }
    /** `name` is cut to 15 bytes, never mid-character. */
    static Command setBatch(uint32_t start_unix_s, const char * name)
    {
        Command x;
        x.kind = Kind::SetBatch;
        x.value = static_cast<int32_t>(start_unix_s);
        size_t n = 0;
        while(name && name[n] && n < State::kBatchLen - 1) n++;
        if(name && name[n])   /* cut short: don't leave half a UTF-8 character */
            while(n > 0 && (static_cast<unsigned char>(name[n]) & 0xC0) == 0x80) n--;
        for(size_t i = 0; i < n; i++) x.name[i] = name[i];
        x.name[n] = '\0';
        return x;
    }
};

/**
 * History samples, oldest first. `reset` starts a new series (full sync); otherwise the
 * samples are appended. A full sync is one reset chunk followed by plain chunks; every chunk
 * but the last has `more` set.
 */
struct HistoryChunk {
    static constexpr int kMax = 24;

    bool     reset = false;
    bool     more = false;            // more chunks of the same sync follow
    uint32_t ms_per_sample = 0;       // meaningful when reset
    uint8_t n = 0;
    float   temps_c[kMax] = {};
};

}  // namespace brew
