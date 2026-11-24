/**
 * BrewDial test harness: the firmware's display side and controller side, wired to simulated
 * hardware, with every knob the web page needs to poke at them.
 *
 * Exactly what runs on the boards runs here: brew::Controller + DeadbandLaw + OutputGuard on
 * the controller side; KnobInput + TouchTracker + UiBridge + LvglPort + brew_ui on the display
 * side; LocalBus between them, or (split mode) StreamPacketLink + Remote*Proxy over a SimWire.
 * Only the drivers are fakes (sim_devices.h).
 *
 * Two clocks:
 *   ui time   real milliseconds from the caller (LVGL animations, knob debounce, link resync)
 *   sim time  ui time x speed (controller, beer physics, relay timings, history)
 */
#pragma once

#include "sim_devices.h"
#include "../sim_panel.h"

#include "brew/app/lvgl_port.h"
#include "brew/app/ui_bridge.h"
#include "brew/control.h"
#include "brew/controller.h"
#include "brew/fake_plant.h"
#include "brew/input.h"
#include "brew/link.h"
#include "brew/local_bus.h"
#include "brew/network/setup_portal.h"
#include "brew/network/wifi_setup.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace brew::sim {

class Harness {
public:
    /** Controller-side settings; applied when the simulated controller (re)boots. */
    struct ControllerSetup {
        /* Defaults = ChamberDefaults, what the boards run. */
        int      law = 1;               // 0 = DeadbandLaw, 1 = HysteresisLaw
        float    heat_diff_f = ChamberDefaults::kHeatDiffF;
        float    cool_diff_f = ChamberDefaults::kCoolDiffF;
        uint32_t heat_min_on_s = 0, heat_min_off_s = ChamberDefaults::kRestartDelayMs / 1000u;
        uint32_t cool_min_on_s = 0, cool_min_off_s = ChamberDefaults::kRestartDelayMs / 1000u;
        uint32_t changeover_s = ChamberDefaults::kChangeoverMs / 1000u;
        uint32_t history_period_ms = 200;
    };

    explicit Harness(int size);
    ~Harness();

    /** Builds LVGL, the UI and the controller. `ui_now` = caller's millisecond clock. */
    void start(uint32_t ui_now);
    /** Advance both sides to `ui_now` and render. Call once per animation frame. */
    void frame(uint32_t ui_now);

    /* ---- display-side inputs ---- */
    void turn(int detents);          // whole detents (+ = clockwise)
    void encoderCounts(int counts);  // raw quadrature counts (try 1 to see bounce rejection)
    void button(bool down);          // knob switch level
    void click();                    // a ~80 ms press
    void touch(int x, int y, bool down);
    /** Square-panel keys (ButtonInput): 0 = up, 1 = down, 2 = select. Level, so hold works. */
    void key(int which, bool down);
    /** Which screen layout the display draws: false = round dial, true = square panel. Swapped
     *  live; what's on screen (and an adjustment in progress) carries over. */
    void setSquare(bool square);
    /** How the display draws the history graph: 0 = hairline (default), 1 = mist. */
    void setGraphStyle(int style);

    /* ---- beer / probe ---- */
    void setBeerC(float c);
    void setPhysics(bool on);
    /** The fermenter's physical setup (volume, insulation, element powers and lags, yeast heat).
     *  Takes effect immediately; the beer keeps its current temperature. */
    void setPlant(const FakePlant::Config & cfg);
    const FakePlant::Config & plant() const { return plant_cfg_; }
    void setProbeFault(int fault);   // SimProbe::Fault
    void setNoiseC(float c);
    void setOffsetC(float c);

    /* ---- controller ---- */
    void setSpeed(float x);
    void rebootController();          // settings survive (like NVS), RAM history doesn't
    void factoryReset();              // wipe settings, then reboot
    ControllerSetup & setup() { return setup_; }

    /* ---- link ---- */
    void setSplit(bool split);
    SimWire & wire() { return wire_; }

    /* ---- WiFi (the display board's), on a fake radio ---- */
    /** A request to the device's setup page, as a phone on its setup network would make. */
    void http(const net::HttpRequest & req, net::HttpResponse & res);
    void setWifiPhone(bool on);       // a phone joins / leaves the setup network
    void setWifiRouter(bool up);      // the home router on / off
    void wifiStartSetup();            // the WiFi menu item
    void wifiCancelSetup();           // a press on the dial while setup shows
    void wifiForget();                // forget the saved network (setup opens again)

    /* ---- outputs ---- */
    const uint8_t * rgba();           // size x size x 4, updated when the screen changed
    uint32_t frameCount() const { return panel_.flushes; }
    std::string statusJson() const;
    bool nextLog(std::string & line);
    int size() const { return size_; }

    static uint32_t lvTick();

private:
    class UiFront;
    class CtlFront;
    struct Scheduled {
        uint32_t at;
        bool down;
    };

    void buildController();
    void advanceSim(float sim_ms);
    void log(const char * fmt, ...);
    void watchController();
    void watchWifi();

    int size_;
    SimPanel panel_;
    std::vector<uint8_t> rgba_;
    uint32_t rgba_at_ = UINT32_MAX;

    /* clocks */
    uint32_t ui_ms_ = 0;
    uint32_t sim_ms_ = 0;
    float sim_frac_ = 0.0f;
    float speed_ = 1.0f;
    bool started_ = false;

    /* display side */
    SimEncoder encoder_;
    SimButton button_;
    std::deque<Scheduled> button_q_;
    KnobInput knob_;
    SimButton keys_[3];              // up, down, select (a square panel's buttons)
    ButtonInput buttons_;
    TouchTracker touch_;
    app::LvglPort lvgl_;
    std::vector<uint8_t> buf1_, buf2_;
    std::unique_ptr<UiFront> ui_front_;
    std::unique_ptr<app::UiBridge> bridge_;

    /* display side: WiFi */
    SimWifi wifi_{ ui_ms_ };
    std::unique_ptr<net::WifiSetup> wifi_setup_;
    std::unique_ptr<net::SetupPortal> portal_;
    net::SetupPhase last_wifi_phase_ = net::SetupPhase::Off;
    net::SetupError last_wifi_error_ = net::SetupError::None;
    bool last_ap_ = false;
    std::string last_ap_pw_;
    int last_clients_ = 0;

    /* transport */
    bool split_ = false;
    LocalBus bus_;
    SimWire wire_;
    StreamPacketLink display_link_{ wire_.a() };
    StreamPacketLink controller_link_{ wire_.b() };
    RemoteControllerProxy remote_ctl_{ display_link_ };
    RemoteUiProxy remote_ui_{ controller_link_ };

    /* controller side */
    FakePlant::Config plant_cfg_;
    FakePlant plant_;
    SimProbe probe_{ plant_ };
    SimRelay heat_{ sim_ms_ }, cool_{ sim_ms_ };
    MemSettings settings_;
    ControllerSetup setup_;
    std::unique_ptr<CtlFront> ctl_front_;
    std::unique_ptr<ControlLaw> law_;
    std::unique_ptr<OutputGuard> guard_;
    std::unique_ptr<Controller> ctl_;
    uint32_t boots_ = 0;

    /* change tracking for the log */
    Action last_action_ = Action::Idle;
    bool last_valid_ = true;

    std::deque<std::string> log_;
};

}  // namespace brew::sim
