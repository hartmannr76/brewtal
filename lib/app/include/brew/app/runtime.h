/**
 * ESP32 display runtime: starts the input and UI tasks. Board main.cpp files build their
 * drivers, run the Controller with drivers::startPeriodic() (if the board has one), then call
 * startDisplay() (if the board has a screen).
 *
 *   input  (core 0, 5 ms)   KnobInput + TouchTracker -> InputEvent ring
 *   ui     (core 1)         owns LVGL: InputEvents + UiPort -> brew_ui -> Commands
 *   ctl    (core 0, 20 ms)  Controller::tick (sensors, law, guard, publish)  [board main]
 *
 * Single board: both share a LocalBus. Split boards: the display gets a
 * RemoteControllerProxy, the controller a RemoteUiProxy (brew/link.h).
 */
#pragma once

#include "brew/input.h"
#include "brew/ports.h"
#include "brew/drivers/interfaces/clock.h"
#include "brew/drivers/interfaces/display.h"
#include "brew/drivers/interfaces/input.h"
#include "brew/drivers/interfaces/http.h"
#include "brew/drivers/interfaces/io.h"
#include "brew/network/setup_portal.h"
#include "brew/network/wifi_setup.h"
#include "lvgl.h"

namespace brew::app {

class UiBridge;

struct DisplayConfig {
    hal::DisplayPanel * panel = nullptr;      // required
    hal::Backlight *    backlight = nullptr;
    uint8_t             backlight_level = 204;
    hal::Touch *        touch = nullptr;
    hal::Encoder *      encoder = nullptr;
    hal::DigitalIn *    button = nullptr;     // knob push switch
    KnobInput::Config   knob = {};
    uint32_t            refresh_ms = 16;      // LVGL refresh period
    /** Runs once after the first frame is on the panel, right after the backlight comes on. */
    void (*after_first_frame)() = nullptr;

    /* The display's WiFi (optional). startDisplay() begins it (it opens setup when nothing is
     * saved) and the setup page's server; the UI task then ticks it and answers the page, so
     * WifiSetup is only ever touched from that one task. */
    net::WifiSetup *   wifi = nullptr;
    net::SetupPortal * portal = nullptr;
    hal::HttpServer *  http = nullptr;
    hal::WallClock *   clock = nullptr;   // internet time, passed on to the controller
    /** Brewfather logging (needs wifi): named after the display's setup network
     *  ("BrewDial-CB50" -> device "BrewDialCB50"), ticked by the UI task, its page added to
     *  `portal`. Its HTTP client must have been begun. */
    net::Brewfather *  brewfather = nullptr;

    /** Optional extra work in the UI task; normal builds leave these empty (the benchmark build,
     *  boards/bench/bench_hooks.cpp, uses them). ui_started runs once after the UI is built,
     *  ui_loop every loop iteration just before LVGL runs. */
    void (*ui_started)(UiBridge & ui, lv_display_t * disp, net::WifiSetup * wifi) = nullptr;
    void (*ui_loop)(uint32_t now_ms) = nullptr;
};

/** Sets up LVGL + brew_ui on `cfg.panel`, then starts the input and UI tasks. */
void startDisplay(const DisplayConfig & cfg, UiPort & port);

}  // namespace brew::app
