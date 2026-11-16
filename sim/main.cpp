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
DeadbandLaw law(0.3f * 5.0f / 9.0f);
OutputGuard guard(&plant.heat(), &plant.cool(), {});
Controller::Config ctlConfig()
{
    Controller::Config c;
    c.day = 4;
    return c;
}
Controller ctl(bus, plant, law, guard, nullptr, ctlConfig());
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

    /* history */
    turn(2);
    run(400);
    press();
    run(600);
    snap("07_history_now");
    turn(-14);
    run(600);
    snap("08_history_scrubbed");
    press();
    run(600);

    /* heating */
    setHistory(66.1f, 68.0f, -2.2f);
    setTemp(66.1f);
    run(1200);
    snap("09_heating");

    /* cold crash via menu */
    setHistory(41.2f, 34.0f, 22.0f);
    setTemp(41.2f);
    press(); run(500);
    turn(1); run(500);
    press(); run(1200);
    snap("10_cold_crash");

    /* pause */
    press(); run(500);
    turn(2); run(500);
    press(); run(1200);
    snap("11_paused");

    /* resume + celsius */
    press(); run(500);
    turn(2); run(500);
    press(); run(1000);           /* resume */
    press(); run(500);
    turn(4); run(500);
    press(); run(300);            /* toggle units */
    snap("12a_menu_units_celsius");
    turn(-4); run(500);
    press(); run(1200);           /* back to ferment */
    snap("12_ferment_celsius");

    const State & s = ctl.state();
    std::printf("final controller state: mode=%d paused=%d celsius=%d sp=%.1f/%.1f F action=%d\n",
                static_cast<int>(s.mode), s.paused, s.celsius, c_to_f(s.setpoint_c[0]), c_to_f(s.setpoint_c[1]),
                static_cast<int>(s.action));
    return 0;
}
