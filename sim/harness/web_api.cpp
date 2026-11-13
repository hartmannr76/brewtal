/**
 * Flat C API over brew::sim::Harness for the web page (sim/web/app.js calls these through the
 * Emscripten module as Module._bd_*). Builds natively too, so harness_check exercises the
 * exact same entry points.
 */
#include "harness.h"

#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define BD_API extern "C" EMSCRIPTEN_KEEPALIVE
#else
#define BD_API extern "C"
#endif

namespace {
brew::sim::Harness * H = nullptr;
std::string g_text;
}  // namespace

BD_API int bd_size() { return BREW_UI_DISP_SIZE; }

BD_API void bd_start(double now_ms)
{
    if(H) return;
    H = new brew::sim::Harness(BREW_UI_DISP_SIZE);
    H->start(static_cast<uint32_t>(now_ms));
}

BD_API void bd_frame(double now_ms) { H->frame(static_cast<uint32_t>(now_ms)); }
BD_API const uint8_t * bd_rgba() { return H->rgba(); }
BD_API uint32_t bd_frame_count() { return H->frameCount(); }

/* display side */
BD_API void bd_turn(int detents) { H->turn(detents); }
BD_API void bd_counts(int counts) { H->encoderCounts(counts); }
BD_API void bd_button(int down) { H->button(down != 0); }
BD_API void bd_click() { H->click(); }
/* square-panel keys: which 0 = up, 1 = down, 2 = select; down = level (hold to repeat) */
BD_API void bd_key(int which, int down) { H->key(which, down != 0); }
BD_API void bd_touch(int x, int y, int down) { H->touch(x, y, down != 0); }

/* beer / probe */
BD_API void bd_set_beer_c(float c) { H->setBeerC(c); }
BD_API void bd_set_physics(int on) { H->setPhysics(on != 0); }
/* the fermenter's physical setup; see FakePlant for what each term means */
BD_API void bd_plant(float volume_l, float ambient_c, float loss_w_per_k, float heat_w, float cool_w, float heat_lag_s,
                     float cool_lag_s, float yeast_w)
{
    auto pos = [](float v) { return v < 0.0f ? 0.0f : v; };
    brew::FakePlant::Config c = H->plant();
    c.volume_l = volume_l < 0.5f ? 0.5f : volume_l;
    c.ambient_c = ambient_c;
    c.loss_w_per_k = pos(loss_w_per_k);
    c.heat_w = pos(heat_w);
    c.cool_w = pos(cool_w);
    c.heat_lag_s = pos(heat_lag_s);
    c.cool_lag_s = pos(cool_lag_s);
    c.yeast_w = yeast_w;   // negative is allowed: e.g. evaporative losses
    H->setPlant(c);
}
BD_API void bd_set_fault(int f) { H->setProbeFault(f); }
BD_API void bd_set_noise(float c) { H->setNoiseC(c); }
BD_API void bd_set_offset(float c) { H->setOffsetC(c); }

/* controller */
BD_API void bd_set_speed(float x) { H->setSpeed(x); }
BD_API void bd_reboot() { H->rebootController(); }
BD_API void bd_factory_reset() { H->factoryReset(); }
BD_API void bd_setup(int law, float heat_diff_f, float cool_diff_f, int heat_min_on_s, int heat_min_off_s,
                     int cool_min_on_s, int cool_min_off_s, int changeover_s, int history_min)
{
    auto nn = [](int v) { return static_cast<uint32_t>(v < 0 ? 0 : v); };
    auto & s = H->setup();
    s.law = law;
    s.heat_diff_f = heat_diff_f;
    s.cool_diff_f = cool_diff_f;
    s.heat_min_on_s = nn(heat_min_on_s);
    s.heat_min_off_s = nn(heat_min_off_s);
    s.cool_min_on_s = nn(cool_min_on_s);
    s.cool_min_off_s = nn(cool_min_off_s);
    s.changeover_s = nn(changeover_s);
    s.history_minutes = static_cast<uint16_t>(history_min < 1 ? 1 : history_min);
    H->rebootController();
}

/* link */
BD_API void bd_set_split(int on) { H->setSplit(on != 0); }
BD_API void bd_link(int unplugged, float drop, float corrupt)
{
    auto & w = H->wire();
    w.unplugged = unplugged != 0;
    w.drop_rate = drop;
    w.corrupt_rate = corrupt;
}

/* readouts (pointers stay valid until the next call of the same kind) */
BD_API const char * bd_status()
{
    g_text = H->statusJson();
    return g_text.c_str();
}

BD_API const char * bd_next_log()
{
    static std::string line;
    if(!H->nextLog(line)) line.clear();
    return line.c_str();
}
