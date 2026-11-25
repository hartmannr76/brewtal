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
int bd_http(const char *, const char *, const char *);
const char * bd_http_body();
const char * bd_http_location();
void bd_wifi_phone(int);
void bd_wifi_router(int);
void bd_wifi_setup();
void bd_wifi_skip();
void bd_factory_reset();
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

/* String field: "key":"value" after "object":. */
std::string sfield(const char * obj, const char * key)
{
    const std::string s = bd_status();
    const size_t at = s.find(std::string("\"") + obj + "\":");
    if(at == std::string::npos) return "?";
    const std::string k = std::string("\"") + key + "\":\"";
    const size_t i = s.find(k, at);
    if(i == std::string::npos) return "?";
    const size_t v = i + k.size();
    return s.substr(v, s.find('"', v) - v);
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

    std::printf("internet time reached the controller\n");
    check(field("state", "clock_unix") == 1, "day count runs on Unix time");
    {
        const double expect = 1795132800.0 + field(nullptr, "sim_ms") / 1000.0;   // SimWallClock
        check(std::abs(field("state", "clock_s") - expect) < 65.0, "controller clock is the internet time (within a minute)");
    }

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
    /* At 10% byte loss a 19-byte command and the State that confirms it both get through only now
     * and then, so how long it takes depends on the loss pattern. What must hold: it crosses before
     * the display stops re-sending (RemoteControllerProxy give_up_ms, 15 s). */
    for(int t = 0; t < 15000 && std::abs(f(field("state", "sp")) - 67.0) >= 0.01; t += 250) run_ms(250);
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

    std::printf("wifi: first start opens setup; a phone joins; wrong password; then connects\n");
    bd_factory_reset();   // nothing saved
    run_ms(200);
    check(sfield("wifi", "phase") == "hotspot", "setup network up on first start");
    check(sfield("wifi", "ap_ssid") == "BrewDial-4F2A", "named after the device");
    check(sfield("wifi", "ap_password").size() >= 8, "with a WPA2-length password");
    bd_wifi_phone(1);
    run_ms(1700);         // the scan takes 1.5 s
    check(sfield("wifi", "phase") == "phone_joined", "the phone is on it");
    check(bd_http("GET", "/hotspot-detect.html", "") == 302 &&
          std::string(bd_http_location()) == "http://192.168.4.1/", "the phone's internet check is sent to the page");
    check(bd_http("GET", "/", "") == 200 && std::string(bd_http_body()).find("Choose your WiFi") != std::string::npos,
          "the setup page is served");
    bd_http("GET", "/api/state", "");
    check(std::string(bd_http_body()).find("\"Hartmann-5G\"") != std::string::npos, "and lists the networks");
    bd_http("POST", "/api/connect", "ssid=Hartmann-5G&password=wrong");
    run_ms(3000);
    check(sfield("wifi", "error") == "wrong_password" && sfield("wifi", "phase") == "phone_joined",
          "wrong password: back to the form, setup still open");
    bd_http("POST", "/api/connect", "ssid=Hartmann-5G&password=hops2026");
    run_ms(200);
    check(sfield("wifi", "phase") == "connecting", "connecting");
    run_ms(3000);
    check(sfield("wifi", "phase") == "connected" && sfield("wifi", "joined") == "Hartmann-5G", "connected");
    check(field("wifi", "ap") == 1, "the setup network lingers so the page can say so");
    run_ms(10500);
    check(field("wifi", "ap") == 0 && field("wifi", "phone") == 0, "then closes, and the phone falls off");
    drain_log();

    std::printf("wifi: the router drops out and comes back\n");
    bd_wifi_router(0);
    run_ms(200);
    check(sfield("wifi", "phase") == "offline", "offline, retrying in the background");
    check(field("wifi", "ap") == 0, "without reopening setup");
    bd_wifi_router(1);
    run_ms(8000);         // next retry after 5 s, joining takes 2.5 s
    check(sfield("wifi", "phase") == "connected", "back on Hartmann-5G by itself");
    bd_reboot();
    run_ms(500);
    check(sfield("wifi", "phase") == "connected", "a controller reboot doesn't touch the display's WiFi");
    drain_log();

    std::printf("wifi: setup from the menu, then skipped\n");
    bd_wifi_setup();
    run_ms(100);
    check(sfield("wifi", "phase") == "hotspot" && field("wifi", "ap") == 1, "the setup network opens again");
    bd_wifi_skip();
    run_ms(100);
    check(sfield("wifi", "phase") == "connected" && field("wifi", "ap") == 0, "skipping keeps the saved network");
    drain_log();

    std::printf("brewfather: credentials from the page, account check\n");
    check(bd_http("GET", "/api/brewfather", "") == 200 && std::string(bd_http_body()).find("\"account\":\"not_set_up\"") != std::string::npos,
          "not set up at first");
    bd_http("POST", "/api/brewfather", "user=uid&key=wrong&stream=S1");
    run_ms(2000);
    bd_http("GET", "/api/brewfather", "");
    check(std::string(bd_http_body()).find("\"account\":\"rejected\"") != std::string::npos, "a wrong key is refused");
    bd_http("POST", "/api/brewfather", "key=secret");
    run_ms(2000);
    bd_http("GET", "/api/brewfather", "");
    std::string bf = bd_http_body();
    check(bf.find("\"account\":\"connected\"") != std::string::npos, "the right key connects");
    check(bf.find("\"attached\":false") != std::string::npos, "nothing posted yet, so not on a batch");
    check(bf.find("secret") == std::string::npos && bf.find("\"uid\"") == std::string::npos, "credentials never sent back");
    run_ms(25000);
    bd_http("GET", "/api/brewfather", "");
    bf = bd_http_body();
    check(bf.find("\"last_post_http\":0") != std::string::npos && bf.find("\"logging\":\"off\"") != std::string::npos,
          "nothing posted until a Brewfather profile runs (it'd land on the wrong batch)");
    drain_log();

    std::printf("brewfather: import #18's profile with the knob (Menu > Settings > Profile > Brewfather)\n");
    auto pick = [](int detents) {   // turn to a row, press it
        if(detents) bd_turn(detents);
        run_ms(400);
        bd_click();
        run_ms(500);
    };
    pick(0);          // home -> menu
    pick(3);          // Settings
    pick(4);          // Profile
    pick(2);          // Brewfather
    pick(0);          // #18 Pale Ale: fetched for a preview
    run_ms(1500);
    pick(2);          // Run this profile (after its two steps)
    run_ms(1500);
    check(sfield("state", "batch") == "Pale Ale", "the dial shows the batch: Pale Ale");
    check(field("state", "day") == 4, "Day 4: fermentation started 3 days ago in Brewfather");
    check(std::abs(f(field("state", "sp")) - 66.0) < 0.05, "Primary's 66 F is the target");
    drain_log();

    std::printf("brewfather: logging to the batch that's running\n");
    run_ms(25000);        // the first post, 20 s after the import
    run_ms(95000);        // and the check 90 s after it
    bd_http("GET", "/api/brewfather", "");
    bf = bd_http_body();
    check(bf.find("\"logging\":\"attached\"") != std::string::npos && bf.find("\"imported\":\"#18 Pale Ale\"") != std::string::npos,
          "readings land on #18 Pale Ale, the batch running");
    check(bf.find("\"last_post_http\":200") != std::string::npos, "the post was accepted");
    drain_log();

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
