/**
 * Frame statistics and the on-device UI benchmark.
 *
 * FrameMeter listens to LVGL's display events and adds every drawn frame (render + flush time,
 * area invalidated) to the windows registered with it, so several readers (the heartbeat, the
 * benchmark) can each keep their own totals.
 *
 * Bench drives the UI through UiBridge::handle() exactly as the knob does, through a fixed
 * script, and logs one line per phase:
 *
 *   BENCH phase=turn_down ms=4801 frames=118 fps=24.6 avg_ms=23 max_ms=41 px_per_frame=80112
 *   BENCH done
 *
 * tools/device_bench.py flashes a bench build, reads these lines and keeps a history.
 * Runs in the LVGL (UI) task only.
 */
#pragma once

#include "lvgl.h"

#include <cstdint>

namespace brew::net {
class WifiSetup;
}

namespace brew::app {

class UiBridge;

struct FrameWindow {
    uint32_t frames = 0;
    uint64_t sum_us = 0;
    uint32_t max_us = 0;
    uint64_t inv_px = 0;   // area invalidated (overlaps count twice)
    void clear() { *this = FrameWindow{}; }
};

class FrameMeter {
public:
    using MicrosFn = int64_t (*)();
    static constexpr int kMaxWindows = 4;

    void attach(lv_display_t * disp, MicrosFn micros);
    bool add(FrameWindow * w);

private:
    static void onEvent(lv_event_t * e);

    MicrosFn micros_ = nullptr;
    FrameWindow * win_[kMaxWindows] = {};
    int64_t start_us_ = 0;
    bool rendered_ = false;
};

class Bench {
public:
    using LogFn = void (*)(const char * line);

    Bench(UiBridge & ui, FrameMeter & meter, LogFn log, net::WifiSetup * wifi = nullptr);

    /** Call every UI loop iteration, before lv_timer_handler(). */
    void step(uint32_t now_ms);
    bool done() const { return phase_ >= kPhaseCount; }

    static constexpr uint32_t kWarmupMs = 4000;   // boot settles (backgrounds finish rendering)

private:
    enum Phase : int { Warmup, Idle, TurnUp, SettleUp, TurnDown, SettleDown, Menu, FullRedraw, kPhaseCount };
    void begin(Phase p, uint32_t now);
    void finish(uint32_t now);

    UiBridge & ui_;
    LogFn log_;
    net::WifiSetup * wifi_;
    FrameWindow win_;
    int phase_ = -1;
    uint32_t t0_ = 0;     // phase start
    int acts_ = 0;        // scripted actions done in this phase
};

}  // namespace brew::app
