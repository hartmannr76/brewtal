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
#include "brew/drivers/interfaces/display.h"
#include "brew/drivers/interfaces/input.h"
#include "brew/drivers/interfaces/io.h"

namespace brew::app {

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
};

/** Sets up LVGL + brew_ui on `cfg.panel`, then starts the input and UI tasks. */
void startDisplay(const DisplayConfig & cfg, UiPort & port);

}  // namespace brew::app
