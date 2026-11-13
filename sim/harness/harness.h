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
        uint32_t heat_min_on_s = 0, heat_min_off_s = 0;
        uint32_t cool_min_on_s = 0, cool_min_off_s = ChamberDefaults::kCoolMinOffMs / 1000u;
        uint32_t changeover_s = ChamberDefaults::kChangeoverMs / 1000u;
        uint16_t history_minutes = 1;
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
    TouchTracker touch_;
    app::LvglPort lvgl_;
    std::vector<uint8_t> buf1_, buf2_;
    std::unique_ptr<UiFront> ui_front_;
    std::unique_ptr<app::UiBridge> bridge_;

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
