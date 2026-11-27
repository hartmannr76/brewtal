/**
 * Headless simulator: the real Controller + LocalBus + UiBridge + LvglPort, driven through a
 * scripted session. Dumps RGB565 frames to <outdir>/<name>.raw (sim/to_png.py makes PNGs).
 * Usage: ./brew_sim [outdir] [round|square]
 */
#include "brew/app/lvgl_port.h"
#include "brew/app/ui_bridge.h"
#include "brew/controller.h"
#include "brew/fake_plant.h"
#include "brew/local_bus.h"
#include "sim_panel.h"

#include "brew_layout.h"
#include "brew_ui.h"
#include "lvgl.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace brew;

namespace {

constexpr int N = BREW_UI_DISP_SIZE;

uint32_t s_ms;
const char * s_out = "out";

uint32_t tick_cb() { return s_ms; }

/* Logs commands like the firmware does, then forwards them. */
class LoggingPort final : public UiPort {
public:
    explicit LoggingPort(UiPort & in) : in_(in) {}
    void send(const Command & c) override
    {
        if(c.kind == Command::Kind::SetSetpoint)
            std::printf("    cmd: setpoint[%d] = %.1f F\n", static_cast<int>(c.mode), c_to_f(c.setpoint_c));
        else if(c.kind == Command::Kind::SetMode)
            std::printf("    cmd: mode=%d paused=%d\n", static_cast<int>(c.mode), c.paused);
        else if(c.kind == Command::Kind::SetUnits)
            std::printf("    cmd: celsius=%d\n", c.celsius);
        in_.send(c);
    }
    bool pollState(State & s) override { return in_.pollState(s); }
    bool pollHistory(HistoryChunk & h) override { return in_.pollHistory(h); }
    bool pollSettings(Settings & s) override { return in_.pollSettings(s); }

private:
    UiPort & in_;
};

/* A frozen plant: the script sets every temperature explicitly so the frames are deterministic. */
FakePlant::Config frozen()
{
    FakePlant::Config c;
    c.start_c = f_to_c(68.4f);
    c.frozen = true;
    c.quantum_c = 0.0f;
    return c;
}

sim::SimPanel panel(N, N);
LocalBus bus;
LoggingPort port(bus);
FakePlant plant(frozen());
/* The plant, read as a plain probe: these frames show the UI (README screenshots), not a demo build,
 * so no DEMO pill. */
class ProbeView final : public hal::TempSensor {
public:
    explicit ProbeView(hal::TempSensor & s) : s_(s) {}
    bool begin() override { return s_.begin(); }
    void update(uint32_t now_ms) override { s_.update(now_ms); }
    hal::TempReading latest() const override { return s_.latest(); }
    const char * name() const override { return s_.name(); }

private:
    hal::TempSensor & s_;
};
ProbeView probe(plant);
DeadbandLaw law(0.3f * 5.0f / 9.0f);
OutputGuard guard(&plant.heat(), &plant.cool(), {});
Controller::Config ctlConfig()
{
    Controller::Config c;
    return c;
}
Controller ctl(bus, probe, law, guard, nullptr, ctlConfig());
app::UiBridge bridge(port);

void run(uint32_t ms)
{
    for(uint32_t t = 0; t < ms; t += 5) {
        s_ms += 5;
        ctl.tick(s_ms);
        bridge.poll(s_ms);
        lv_timer_handler();
    }
}

void snap(const char * name)
{
    lv_refr_now(nullptr);
    char p[256];
    std::snprintf(p, sizeof(p), "%s/%s.raw", s_out, name);
    FILE * f = std::fopen(p, "wb");
    if(!f) {
        std::perror(p);
        std::exit(1);
    }
    std::fwrite(panel.framebuffer(), 2, static_cast<size_t>(N) * N, f);
    std::fclose(f);
    std::printf("  %s\n", name);
}

void turn(int d) { bridge.handle(InputEvent::rotate(d)); }
void press() { bridge.handle(InputEvent::press()); }

/* Turn to a row by its label, so the script doesn't break when a list gains rows. */
void go(const char * label)
{
    if(!brew_ui_go(label)) {
        std::fprintf(stderr, "brew_sim: no \"%s\" row here; the script is out of date\n", label);
        std::exit(1);
    }
}
/* Go to a row and press it. */
void pick(const char * label, uint32_t settle_ms = 500)
{
    go(label);
    run(300);
    press();
    run(settle_ms);
}

void setTemp(float f)
{
    plant.set(f_to_c(f));
    ctl.evaluateNow(s_ms);
}

void setHistory(float end, float target, float swing)
{
    seedDemoHistory(ctl.history(), 48, end, target, swing);
    ctl.syncHistory();
}

}  // namespace

int main(int argc, char ** argv)
{
    if(argc > 1) s_out = argv[1];
    const bool square = argc > 2 && std::strcmp(argv[2], "square") == 0;
    lv_init();
    lv_tick_set_cb(tick_cb);
    static uint16_t buf[N * 60];
    app::LvglPort lvgl;
    lvgl.begin(panel, buf, nullptr, sizeof(buf), 0);

    seedDemoHistory(ctl.history(), 48, 68.4f, 66.0f, 2.6f);
    ctl.begin(s_ms);
    bridge.create(lv_screen_active(), square ? &brew_layout_square : nullptr);   // asks for a full sync
    run(800);
    snap("01_home_cooling");

    /* turn the crown: digit animation mid-flight, then settled */
    turn(1);
    run(70);
    snap("02a_digit_anim_midflight");
    run(400);
    turn(1); run(120);
    turn(1); run(120);
    turn(1);
    run(600);
    snap("02_adjusting_target");

    run(3000); /* auto-commit after idle */
    snap("03_after_commit");

    /* temperature reaches target -> holding */
    setTemp(68.0f);
    run(2500);
    snap("04_holding");

    /* menu */
    press();
    run(600);
    snap("05_menu");
    turn(1);
    run(110);
    snap("05b_menu_scrolling");
    run(500);
    snap("06_menu_cold_crash_selected");

    /* settings: heat differential */
    pick("Settings", 600);
    snap("07_settings");
    pick("Heat", 600);
    snap("08_settings_heat");
    press();                      /* edit the differential */
    run(600);
    turn(3);
    run(400);
    snap("08b_edit_heat_differential");
    press();                      /* save: back to the Heat list */
    run(600);
    pick("Previous");             /* -> Settings */
    pick("Previous");             /* -> menu */
    pick("Previous", 600);        /* -> home */

    /* heating */
    setHistory(66.1f, 68.0f, -2.2f);
    setTemp(66.1f);
    run(1200);
    snap("09_heating");

    /* cold crash via menu */
    setHistory(41.2f, 34.0f, 22.0f);
    setTemp(41.2f);
    press(); run(500);
    pick("Cold crash", 1200);
    snap("10_cold_crash");

    /* pause */
    press(); run(500);
    pick("Pause", 1200);
    snap("11_paused");

    /* resume + celsius */
    press(); run(500);
    pick("Resume", 1000);
    press(); run(500);
    pick("Settings");
    pick("Units", 300);           /* toggles in place */
    snap("12a_settings_units_celsius");
    pick("Previous");             /* -> menu */
    pick("Ferment", 1200);
    snap("12_ferment_celsius");

    const State & s = ctl.state();
    std::printf("final controller state: mode=%d paused=%d celsius=%d sp=%.1f/%.1f F action=%d\n",
                static_cast<int>(s.mode), s.paused, s.celsius, c_to_f(s.setpoint_c[0]), c_to_f(s.setpoint_c[1]),
                static_cast<int>(s.action));
    return 0;
}
