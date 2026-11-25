/**
 * Frame statistics and the on-device UI benchmark.
 *
 * FrameMeter listens to LVGL's display events and adds every drawn frame (render + flush time,
 * area invalidated, area actually rendered) to the windows registered with it, so several
 * readers (the heartbeat, the benchmark) can each keep their own totals. On the device a frame's
 * time is measured; on the host (sim/harness) it's estimated from the rendered area by a cost
 * model fitted to device runs (setCostModel), since the PC's own speed says nothing.
 *
 * Bench drives the UI through UiBridge::handle() exactly as the knob does, through a fixed
 * script, and logs one line per phase:
 *
 *   BENCH phase=turn_down ms=4801 frames=118 fps=24.6 avg_ms=23 max_ms=41 px_per_frame=80112
 *         rendered_px_per_frame=61200
 *   BENCH done
 *
 * tools/device_bench.py flashes a bench build, reads these lines and keeps a history;
 * sim/harness/bench_check.cpp runs the same script on the host for presubmit.
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
    uint64_t inv_px = 0;        // area invalidated (overlaps count twice)
    uint64_t rendered_px = 0;   // area rendered and flushed
    void clear() { *this = FrameWindow{}; }
};

class FrameMeter {
public:
    using MicrosFn = int64_t (*)();
    /** Estimated device time for a frame that rendered `px` pixels (host runs). */
    struct CostModel {
        double base_us = 0.0;
        double us_per_px = 0.0;
    };
    static constexpr int kMaxWindows = 4;

    void attach(lv_display_t * disp, MicrosFn micros);
    bool add(FrameWindow * w);
    void remove(FrameWindow * w);
    /** Frames are timed by the model instead of the clock from now on. */
    void setCostModel(const CostModel & m) { model_ = m; modelled_ = true; }

    /** Frames drawn so far, and the last one's time (us). */
    uint32_t frames() const { return frames_; }
    uint32_t lastFrameUs() const { return last_us_; }

private:
    static void onEvent(lv_event_t * e);

    MicrosFn micros_ = nullptr;
    FrameWindow * win_[kMaxWindows] = {};
    int64_t start_us_ = 0;
    bool rendered_ = false;
    uint64_t frame_px_ = 0;
    uint32_t frames_ = 0, last_us_ = 0;
    CostModel model_;
    bool modelled_ = false;
};

class Bench {
public:
    using LogFn = void (*)(const char * line);

    Bench(UiBridge & ui, FrameMeter & meter, LogFn log, net::WifiSetup * wifi = nullptr);
    ~Bench();
    Bench(const Bench &) = delete;
    Bench & operator=(const Bench &) = delete;

    /** Call every UI loop iteration, before lv_timer_handler(). */
    void step(uint32_t now_ms);
    bool done() const { return phase_ >= kPhaseCount; }

    static constexpr uint32_t kWarmupMs = 4000;   // boot settles (backgrounds finish rendering)

private:
    enum Phase : int { Warmup, Idle, TurnUp, SettleUp, TurnDown, SettleDown, Menu, FullRedraw, kPhaseCount };
    void begin(Phase p, uint32_t now);
    void finish(uint32_t now);

    UiBridge & ui_;
    FrameMeter & meter_;
    LogFn log_;
    net::WifiSetup * wifi_;
    FrameWindow win_;
    int phase_ = -1;
    uint32_t t0_ = 0;     // phase start
    int acts_ = 0;        // scripted actions done in this phase
};

}  // namespace brew::app
