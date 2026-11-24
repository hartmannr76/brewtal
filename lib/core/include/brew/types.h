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
struct Settings {
    float    heat_diff_c = 0.0f;           // heating starts this far below the setpoint
    float    cool_diff_c = 0.0f;           // cooling starts this far above it
    uint16_t heat_delay_s = 0;             // restart delays
    uint16_t cool_delay_s = 0;
    float    crash_default_c = 0.0f;       // where Cold crash starts
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
    };

    Kind  kind = Kind::RequestSync;
    Mode  mode = Mode::Ferment;
    float setpoint_c = 0.0f;
    bool  paused = false;
    bool  celsius = false;
    Tuning tuning = Tuning::HeatDiff;
    int32_t value = 0;

    static Command setSetpoint(Mode m, float c) { Command x; x.kind = Kind::SetSetpoint; x.mode = m; x.setpoint_c = c; return x; }
    static Command setMode(Mode m, bool p)      { Command x; x.kind = Kind::SetMode; x.mode = m; x.paused = p; return x; }
    static Command setUnits(bool c)             { Command x; x.kind = Kind::SetUnits; x.celsius = c; return x; }
    static Command requestSync()                { return Command{}; }
    static Command startDayCount()              { Command x; x.kind = Kind::StartDayCount; return x; }
    static Command setTuning(Tuning t, int32_t v) { Command x; x.kind = Kind::SetTuning; x.tuning = t; x.value = v; return x; }
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
