/**
 * Connects brew_ui (the C view) to a UiPort:
 *   - State / History / Settings from the port -> brew_ui_set_* (only for values that changed)
 *   - brew_ui callbacks -> Commands on the port
 *   - InputEvents (Rotate/Press) -> brew_ui_encoder / brew_ui_press
 *   - the display's WiFi (net::WifiSetup, if attached) -> brew_ui_set_wifi, and the WiFi
 *     screen's choices back to it
 * Converts between core's Celsius and brew_ui's Fahrenheit API. Runs in the LVGL task only.
 */
#pragma once

#include "brew/input.h"
#include "brew/network/wifi_setup.h"
#include "brew/ports.h"

#include "lvgl.h"

struct brew_layout;   // brew_layout.h (brew_ui)

namespace brew::app {

class UiBridge {
public:
    explicit UiBridge(UiPort & port) : port_(port) {}

    /** Builds the UI on `screen` and asks the controller for a full sync. `layout` picks the
     *  screen shape (brew_layout_round, brew_layout_square, ...); nullptr = round. */
    void create(lv_obj_t * screen, const brew_layout * layout = nullptr);

    /** Applies pending State/History. Call every loop iteration, before lv_timer_handler(). */
    void poll(uint32_t now_ms);

    /** Rotate/Press go to the UI. Touch events are ignored here (they go to the LVGL indev). */
    void handle(const InputEvent & e);

    /** The display's WiFi. Without one the UI's WiFi menu item says "unavailable". Same task:
     *  tick the WifiSetup in the loop that calls poll(). */
    void attachWifi(net::WifiSetup * wifi) { wifi_ = wifi; wifi_pushed_ = false; }

    /* brew_ui callback targets (called through extern "C" trampolines; `user` = this) */
    static void onSetpointThunk(int mode, float temp_f, void * user);
    static void onModeThunk(int mode, bool paused, void * user);
    static void onUnitsThunk(bool celsius, void * user);
    static void onResetDayThunk(void * user);
    static void onWifiSetupThunk(void * user);
    static void onWifiCancelThunk(void * user);
    static void onWifiForgetThunk(void * user);
    static void onTuningThunk(int what, float value, void * user);

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
        Action wanted = Action::Idle;
        uint16_t wait_s = 0;
        bool  tuned = false;
        float heat_diff_c = 0.0f, cool_diff_c = 0.0f, crash_c = 0.0f;
        uint16_t heat_delay_s = 0, cool_delay_s = 0;
        char  batch[State::kBatchLen] = { 0 };
        uint16_t day = 0;
    };

    void apply(const State & s);
    void apply(const Settings & s);
    void applyHistory(const HistoryChunk & h);

    void pushWifi();

    UiPort & port_;
    Shown shown_;
    uint32_t now_ = 0;
    net::WifiSetup * wifi_ = nullptr;
    net::SetupStatus wifi_shown_;
    bool wifi_pushed_ = false;
};

}  // namespace brew::app
