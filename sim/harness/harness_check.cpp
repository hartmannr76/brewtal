/**
 * Native smoke test for the web harness: drives the same C API the page uses (bd_*) and
 * checks the whole loop works: knob -> UI -> Command -> controller -> relays -> beer ->
 * probe -> State -> UI, on one board and over the simulated cable. Run by ctest.
 */
#include "harness.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
int bd_size();
void bd_start(double);
void bd_frame(double);
const uint8_t * bd_rgba();
void bd_turn(int);
void bd_click();
void bd_key(int, int);
void bd_layout(int);
void bd_set_beer_c(float);
void bd_set_physics(int);
void bd_set_fault(int);
void bd_plant(float, float, float, float, float, float, float, float);
void bd_set_speed(float);
void bd_reboot();
void bd_setup(int, float, float, int, int, int, int, int, int);
void bd_set_split(int);
void bd_link(int, float, float);
const char * bd_status();
const char * bd_next_log();
}

namespace {

double g_now = 1000.0;
int g_fail = 0;

void run_ms(double ms)
{
    const double end = g_now + ms;
    while(g_now < end) {
        g_now += 16.0;
        bd_frame(g_now);
    }
}

/* Tiny JSON field reader: finds "key": after an optional "object": prefix. */
double field(const char * obj, const char * key)
{
    const std::string s = bd_status();
    size_t at = 0;
    if(obj) {
        at = s.find(std::string("\"") + obj + "\":");
        if(at == std::string::npos) return -9999;
    }
    const size_t k = s.find(std::string("\"") + key + "\":", at);
    if(k == std::string::npos) return -9999;
    const char * v = s.c_str() + k + std::strlen(key) + 3;
    if(!std::strncmp(v, "true", 4)) return 1;
    if(!std::strncmp(v, "false", 5)) return 0;
    if(*v == '[') v++;
    return std::atof(v);
}

void check(bool ok, const char * what)
{
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if(!ok) g_fail++;
}

void drain_log()
{
    for(const char * l = bd_next_log(); *l; l = bd_next_log()) std::printf("      | %s\n", l);
}

double f(double c) { return c * 9.0 / 5.0 + 32.0; }

}  // namespace

int main()
{
    std::printf("harness_check (%dpx)\n", bd_size());
    bd_start(g_now);
    run_ms(1000);
    drain_log();

    std::printf("boot\n");
    check(field("state", "valid") == 1, "controller has a valid reading");
    check(std::abs(f(field("state", "temp_c")) - 68.4) < 0.2, "beer starts at 68.4 F");
    check(std::abs(f(field("state", "sp")) - 66.0) < 0.01, "ferment target defaults to 66 F");
    check(field("state", "action") == 1 && field("cool", "on") == 1,
          "68.4 F vs 66 F: cooling starts right away, nothing is held off at power-up");
    check(field("pending", "wait_s") == 0, "nothing pending");
    check(std::string(bd_status()).find("\"ui_status\":\"Cooling to 66") != std::string::npos,
          "the dial says \"Cooling to 66\"");
    {
        const uint8_t * px = bd_rgba();
        long lit = 0;
        for(int i = 0; i < bd_size() * bd_size(); i++) lit += px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2];
        check(lit > 0, "screen rendered something");
    }

    std::printf("knob: 6 detents up, press\n");
    bd_turn(6);
    run_ms(200);
    bd_click();
    run_ms(500);
    drain_log();
    check(std::abs(f(field("state", "sp")) - 69.0) < 0.01, "controller target is now 69 F");
    check(field("state", "action") == 2, "68.4 F vs 69 F: heating");
    check(field("heat", "on") == 1 && field("cool", "on") == 0, "heat relay on, cool relay off");

    std::printf("time x600 for 20 s (~3.3 h simulated)\n");
    bd_set_speed(600);
    run_ms(20000);
    bd_set_speed(1);
    drain_log();
    check(std::abs(f(field("state", "temp_c")) - 69.0) < 0.6, "beer held near 69 F");
    check(field("heat", "switches") >= 2, "heat relay cycled");

    std::printf("probe disconnected\n");
    bd_set_fault(1);
    run_ms(2000);
    drain_log();
    check(field("state", "valid") == 0, "controller sees an invalid reading");
    check(field("heat", "on") == 0 && field("cool", "on") == 0, "fail safe: both relays off");
    bd_set_fault(0);
    run_ms(2000);
    drain_log();

    std::printf("split boards over the simulated cable, 10%% byte loss\n");
    bd_set_split(1);
    bd_link(0, 0.10f, 0.0f);
    run_ms(4000);
    bd_set_physics(0);
    bd_set_beer_c(15.0f);   // 59 F
    run_ms(4000);
    bd_turn(-4);            // 69 -> 67 F
    run_ms(200);
    bd_click();             // one press: the display re-sends until the controller confirms
    run_ms(6000);
    drain_log();
    check(std::abs(f(field("state", "sp")) - 67.0) < 0.01, "command crossed the lossy cable");
    check(field("state", "action") == 2, "59 F vs 67 F: heating");
    check(field("link", "bytes_lost") > 0, "the cable really dropped bytes");
    check(field("link", "bad_frames_display") + field("link", "bad_frames_controller") > 0,
          "framing rejected damaged packets");

    std::printf("controller reboot keeps settings\n");
    bd_link(0, 0, 0);
    bd_reboot();
    run_ms(3000);
    drain_log();
    check(std::abs(f(field("state", "sp")) - 67.0) < 0.01, "target survived the reboot");
    check(field(nullptr, "boots") == 2, "boot counter");

    std::printf("square-panel keys: tap up, hold up, select\n");
    const double before = f(field("state", "sp"));
    bd_key(0, 1); run_ms(100); bd_key(0, 0); run_ms(200);   // tap: +1 detent
    bd_key(0, 1); run_ms(700); bd_key(0, 0); run_ms(200);   // hold: step at 25 ms, repeats at 475, 595
    bd_key(2, 1); run_ms(100); bd_key(2, 0); run_ms(1500);  // select: commit now
    drain_log();
    /* 0.05: a target read back after a reboot is stored in centi-degC (67 F -> 66.99 F) */
    check(std::abs(f(field("state", "sp")) - (before + 2.0)) < 0.05, "4 half-degree steps committed by select");

    std::printf("layout swap mid-adjustment: turn on the round dial, select on the square panel\n");
    {
        const double sp = f(field("state", "sp"));
        bd_turn(-2);                 // adjusting on the round layout...
        run_ms(200);
        bd_layout(1);                // ...swap to square: the draft and its countdown carry over
        run_ms(200);
        check(std::string(bd_status()).find("\"layout\":\"square\"") != std::string::npos, "square layout active");
        bd_key(2, 1); run_ms(100); bd_key(2, 0); run_ms(1000);   // select commits the draft
        drain_log();
        check(std::abs(f(field("state", "sp")) - (sp - 1.0)) < 0.05, "the draft from the round dial was committed");
        const uint8_t * px = bd_rgba();
        long lit = 0;
        for(int i = 0; i < bd_size() * bd_size(); i++) lit += px[i * 4] | px[i * 4 + 1] | px[i * 4 + 2];
        check(lit > 0, "square screen rendered");
        bd_layout(0);
        run_ms(300);
        check(std::string(bd_status()).find("\"layout\":\"round\"") != std::string::npos, "back to round");
    }

    std::printf("setup applies\n");
    bd_setup(1, 0.5f, 0.75f, 60, 0, 120, 300, 600, 200);
    run_ms(2000);
    drain_log();
    check(field("setup", "law") == 1 && field("setup", "cool_min_off_s") == 300 &&
              std::abs(field("setup", "cool_diff_f") - 0.75) < 1e-6,
          "setup applied");

    std::printf("original deadband vs chamber defaults: active ferment held at 64 F, 12 h each\n");
    bd_set_split(0);
    bd_set_physics(1);
    /* 5 gal, neoprene jacket, 68 F room, 50 W wrap, 150 W glycol coil, 15 W of yeast heat */
    bd_plant(18.9f, 20.0f, 1.8f, 50.0f, 150.0f, 240.0f, 60.0f, 15.0f);
    bd_turn(static_cast<int>(std::lround((64.0 - f(field("state", "sp"))) * 2.0)));   // -> 64 F, half-degree detents
    run_ms(200);
    bd_click();
    run_ms(1000);
    check(std::abs(f(field("state", "sp")) - 64.0) < 0.01, "target 64 F");
    auto soak = [](int law, float diff, int restart, int gap, double * lo, double * hi) {
        bd_setup(law, diff, diff, 0, restart, 0, restart, gap, 200);
        bd_set_beer_c(17.78f);   // start at the target
        bd_set_speed(3600);
        run_ms(3000);           // settle 3 h
        const double before = field("heat", "switches") + field("cool", "switches");
        *lo = 1e9;
        *hi = -1e9;
        for(int i = 0; i < 90; i++) {   // 9 h, sampled every 6 min
            run_ms(100);
            const double t = f(field(nullptr, "beer_c"));
            if(t < *lo) *lo = t;
            if(t > *hi) *hi = t;
        }
        bd_set_speed(1);
        drain_log();
        return field("heat", "switches") + field("cool", "switches") - before;
    };
    double olo, ohi, rlo, rhi;
    const double orig = soak(0, 0.3f, 0, 0, &olo, &ohi);
    const double rec = soak(1, 0.5f, 180, 0, &rlo, &rhi);   // ChamberDefaults
    std::printf("      original:         %3.0f relay switches in 9 h, beer %.2f..%.2f F\n", orig, olo, ohi);
    std::printf("      chamber defaults: %3.0f relay switches in 9 h, beer %.2f..%.2f F\n", rec, rlo, rhi);
    check(orig > 0 && rec > 0, "both had to cool against the yeast heat");
    check(rec * 2 <= orig, "chamber defaults switch the relays far less often");
    check(rlo > 63.0 && rhi < 65.0, "and still hold the beer within 1 F of 64 F");

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
