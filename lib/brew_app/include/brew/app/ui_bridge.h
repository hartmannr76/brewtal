/**
 * Connects brew_ui (the C view) to a UiPort:
 *   - State / History from the port -> brew_ui_set_* (only for values that changed)
 *   - brew_ui callbacks -> Commands on the port
 *   - InputEvents (Rotate/Press) -> brew_ui_encoder / brew_ui_press
 * Converts between core's Celsius and brew_ui's Fahrenheit API. Runs in the LVGL task only.
 */
#pragma once

#include "brew/input.h"
#include "brew/ports.h"

#include "lvgl.h"

namespace brew::app {

class UiBridge {
public:
    explicit UiBridge(UiPort & port) : port_(port) {}

    /** Builds the UI on `screen` and asks the controller for a full sync. */
    void create(lv_obj_t * screen);

    /** Applies pending State/History. Call every loop iteration, before lv_timer_handler(). */
    void poll(uint32_t now_ms);

    /** Rotate/Press go to the UI. Touch events are ignored here (they go to the LVGL indev). */
    void handle(const InputEvent & e);

    /* brew_ui callback targets (called through extern "C" trampolines; `user` = this) */
    static void onSetpointThunk(int mode, float temp_f, void * user);
    static void onModeThunk(int mode, bool paused, void * user);
    static void onUnitsThunk(bool celsius, void * user);

private:
    struct Shown {
        bool  any = false;
        float temp_f = 0.0f;
        bool  temp_set = false;
        float sp_f[kModeCount] = { 0.0f, 0.0f };
        Mode  mode = Mode::Ferment;
        bool  paused = false;
        bool  celsius = false;
        Action action = Action::Idle;
        char  batch[State::kBatchLen] = { 0 };
        uint16_t day = 0;
    };

    void apply(const State & s);
    void applyHistory(const HistoryChunk & h);

    UiPort & port_;
    Shown shown_;
};

}  // namespace brew::app
