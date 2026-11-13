#include "brew/app/ui_bridge.h"

#include "brew_ui.h"

#include <cmath>
#include <cstring>

namespace brew::app {

namespace {
constexpr float kSetpointEpsF = 0.02f;   // C<->F round trips are not exact

brew_mode_t to_ui(Mode m) { return m == Mode::Crash ? BREW_MODE_CRASH : BREW_MODE_FERMENT; }

brew_action_t to_ui(Action a)
{
    switch(a) {
        case Action::Cooling: return BREW_ACTION_COOLING;
        case Action::Heating: return BREW_ACTION_HEATING;
        default:              return BREW_ACTION_IDLE;
    }
}
}  // namespace

/* brew_ui's callback signatures use its own enum; these trampolines keep brew_ui.h C-only. */
extern "C" {
static void cb_setpoint(brew_mode_t m, float f, void * u) { UiBridge::onSetpointThunk(static_cast<int>(m), f, u); }
static void cb_mode(brew_mode_t m, bool p, void * u) { UiBridge::onModeThunk(static_cast<int>(m), p, u); }
static void cb_units(bool c, void * u) { UiBridge::onUnitsThunk(c, u); }
}

void UiBridge::create(lv_obj_t * screen)
{
    brew_ui_cbs_t cbs = {};
    cbs.on_setpoint = cb_setpoint;
    cbs.on_mode = cb_mode;
    cbs.on_units = cb_units;
    cbs.user = this;
    brew_ui_create(screen, &cbs);
    port_.send(Command::requestSync());
}

/* ---- UI -> controller ---- */

void UiBridge::onSetpointThunk(int mode, float temp_f, void * user)
{
    auto * self = static_cast<UiBridge *>(user);
    const Mode m = mode == BREW_MODE_CRASH ? Mode::Crash : Mode::Ferment;
    self->shown_.sp_f[static_cast<int>(m)] = temp_f;   // the UI already shows it
    self->port_.send(Command::setSetpoint(m, f_to_c(temp_f)));
}

void UiBridge::onModeThunk(int mode, bool paused, void * user)
{
    auto * self = static_cast<UiBridge *>(user);
    const Mode m = mode == BREW_MODE_CRASH ? Mode::Crash : Mode::Ferment;
    self->shown_.mode = m;
    self->shown_.paused = paused;
    self->port_.send(Command::setMode(m, paused));
}

void UiBridge::onUnitsThunk(bool celsius, void * user)
{
    auto * self = static_cast<UiBridge *>(user);
    self->shown_.celsius = celsius;
    self->port_.send(Command::setUnits(celsius));
}

/* ---- controller -> UI ---- */

void UiBridge::poll(uint32_t now_ms)
{
    port_.service(now_ms);
    HistoryChunk h;
    while(port_.pollHistory(h)) applyHistory(h);
    State s;
    if(port_.pollState(s)) apply(s);
}

void UiBridge::apply(const State & s)
{
    Shown & v = shown_;
    const bool all = !v.any;
    v.any = true;

    if(all || s.celsius != v.celsius) {
        v.celsius = s.celsius;
        brew_ui_set_units(s.celsius);
    }
    if(all || s.day != v.day || std::strncmp(s.batch, v.batch, sizeof(v.batch)) != 0) {
        std::memcpy(v.batch, s.batch, sizeof(v.batch));
        v.batch[sizeof(v.batch) - 1] = '\0';
        v.day = s.day;
        brew_ui_set_batch(v.batch, v.day);
    }
    const float f0 = c_to_f(s.setpoint_c[0]), f1 = c_to_f(s.setpoint_c[1]);
    if(all || std::fabs(f0 - v.sp_f[0]) > kSetpointEpsF || std::fabs(f1 - v.sp_f[1]) > kSetpointEpsF) {
        /* Snap to the UI's 0.05 F grid so C<->F rounding never shows up as a 0.1 change. */
        v.sp_f[0] = std::round(f0 * 20.0f) / 20.0f;
        v.sp_f[1] = std::round(f1 * 20.0f) / 20.0f;
        brew_ui_set_setpoints(v.sp_f[0], v.sp_f[1]);
    }
    if(all || s.mode != v.mode || s.paused != v.paused) {
        v.mode = s.mode;
        v.paused = s.paused;
        brew_ui_set_mode(to_ui(s.mode), s.paused);
    }
    if(s.temp_valid) {
        const float tf = c_to_f(s.temp_c);
        if(!v.temp_set || std::fabs(tf - v.temp_f) > 0.001f) {
            v.temp_f = tf;
            v.temp_set = true;
            brew_ui_set_temp(tf);
        }
    }
    if(all || s.action != v.action) {
        v.action = s.action;
        brew_ui_set_action(to_ui(s.action));
    }
}

void UiBridge::applyHistory(const HistoryChunk & h)
{
    float f[HistoryChunk::kMax];
    for(int i = 0; i < h.n; i++) f[i] = c_to_f(h.temps_c[i]);
    if(h.reset) {
        brew_ui_set_history(f, h.n, h.minutes_per_sample);
    }
    else {
        for(int i = 0; i < h.n; i++) brew_ui_push_history(f[i]);
    }
}

/* ---- input ---- */

void UiBridge::handle(const InputEvent & e)
{
    switch(e.kind) {
        case InputEvent::Kind::Rotate: brew_ui_encoder(e.a); break;
        case InputEvent::Kind::Press:  brew_ui_press(); break;
        default: break;
    }
}

}  // namespace brew::app
