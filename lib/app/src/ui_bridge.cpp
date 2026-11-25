#include "brew/app/ui_bridge.h"

#include "brew_ui.h"

#include <cmath>
#include <cstdio>
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
static void cb_reset_day(void * u) { UiBridge::onResetDayThunk(u); }
static void cb_wifi_setup(void * u) { UiBridge::onWifiSetupThunk(u); }
static void cb_wifi_cancel(void * u) { UiBridge::onWifiCancelThunk(u); }
static void cb_wifi_forget(void * u) { UiBridge::onWifiForgetThunk(u); }
static void cb_tuning(brew_tuning_t t, float v, void * u) { UiBridge::onTuningThunk(static_cast<int>(t), v, u); }
static void cb_profile(brew_profile_t p, void * u) { UiBridge::onProfileThunk(static_cast<int>(p), u); }
static void cb_step(int i, float t, int r, int h, void * u) { UiBridge::onStepThunk(i, t, r, h, u); }
static void cb_step_count(int n, void * u) { UiBridge::onStepCountThunk(n, u); }
static void cb_bf_check(void * u) { UiBridge::onBfCheckThunk(u); }
static void cb_bf_pick(int i, void * u) { UiBridge::onBfPickThunk(i, u); }
static void cb_bf_run(void * u) { UiBridge::onBfRunThunk(u); }
}

void UiBridge::create(lv_obj_t * screen, const brew_layout * layout)
{
    brew_ui_cbs_t cbs = {};
    cbs.on_setpoint = cb_setpoint;
    cbs.on_mode = cb_mode;
    cbs.on_units = cb_units;
    cbs.on_reset_day = cb_reset_day;
    cbs.on_wifi_setup = cb_wifi_setup;
    cbs.on_wifi_cancel = cb_wifi_cancel;
    cbs.on_wifi_forget = cb_wifi_forget;
    cbs.on_tuning = cb_tuning;
    cbs.on_profile = cb_profile;
    cbs.on_step = cb_step;
    cbs.on_step_count = cb_step_count;
    cbs.on_bf_check = cb_bf_check;
    cbs.on_bf_pick = cb_bf_pick;
    cbs.on_bf_run = cb_bf_run;
    cbs.user = this;
    brew_ui_create_with_layout(screen, &cbs, layout);
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

void UiBridge::onResetDayThunk(void * user)
{
    auto * self = static_cast<UiBridge *>(user);
    self->shown_.day = 1;   // the UI already shows Day 1
    self->port_.send(Command::startDayCount());
}

/* ---- controller -> UI ---- */

void UiBridge::onWifiSetupThunk(void * user)
{
    auto * self = static_cast<UiBridge *>(user);
    if(self->wifi_) self->wifi_->startSetup(self->now_);   // its new status opens the WiFi screen
}

void UiBridge::onWifiCancelThunk(void * user)
{
    auto * self = static_cast<UiBridge *>(user);
    if(self->wifi_) self->wifi_->cancelSetup(self->now_);
}

void UiBridge::onWifiForgetThunk(void * user)
{
    auto * self = static_cast<UiBridge *>(user);
    if(self->wifi_) self->wifi_->forget(self->now_);
}

void UiBridge::onTuningThunk(int what, float value, void * user)
{
    auto * self = static_cast<UiBridge *>(user);
    /* brew_ui speaks F (temperatures and differences) and seconds; the wire, centi-C and seconds */
    auto cc = [](float c) { return static_cast<int32_t>(std::lround(c * 100.0f)); };
    switch(what) {
        case BREW_TUNE_HEAT_DIFF:  self->port_.send(Command::setTuning(Tuning::HeatDiff, cc(value * 5.0f / 9.0f))); break;
        case BREW_TUNE_COOL_DIFF:  self->port_.send(Command::setTuning(Tuning::CoolDiff, cc(value * 5.0f / 9.0f))); break;
        case BREW_TUNE_HEAT_DELAY: self->port_.send(Command::setTuning(Tuning::HeatDelay, static_cast<int32_t>(std::lround(value)))); break;
        case BREW_TUNE_COOL_DELAY: self->port_.send(Command::setTuning(Tuning::CoolDelay, static_cast<int32_t>(std::lround(value)))); break;
        case BREW_TUNE_CRASH:      self->port_.send(Command::setTuning(Tuning::CrashDefault, cc(f_to_c(value)))); break;
        default: break;
    }
}

void UiBridge::onProfileThunk(int profile, void * user)
{
    static_cast<UiBridge *>(user)->port_.send(Command::setProfile(profile == BREW_PROFILE_SCHEDULE ? Profile::Schedule : Profile::Constant));
}

void UiBridge::onStepThunk(int i, float temp_f, int ramp_h, int hold_h, void * user)
{
    if(i < 0 || i >= Settings::kMaxSteps) return;
    static_cast<UiBridge *>(user)->port_.send(Command::setStep(static_cast<uint8_t>(i), f_to_c(temp_f), static_cast<uint16_t>(ramp_h),
                                                               static_cast<uint16_t>(hold_h)));
}

void UiBridge::onBfCheckThunk(void * user)
{
    auto * self = static_cast<UiBridge *>(user);
    if(self->bf_) self->bf_->checkNow(self->now_);
}

void UiBridge::onBfPickThunk(int i, void * user)
{
    auto * self = static_cast<UiBridge *>(user);
    if(self->bf_) self->bf_->fetchBatch(i, self->now_);
}

void UiBridge::onBfRunThunk(void * user) { static_cast<UiBridge *>(user)->importPreview(); }

/* The previewed Brewfather batch into the schedule: its steps, how many, then SetBatch, which
 * starts it as of the batch's fermentation start (the controller does the profile switch). */
void UiBridge::importPreview()
{
    if(!bf_) return;
    const net::BfPreview & p = bf_->status().preview;
    if(p.state != net::BfPreview::State::Ready || p.nsteps < 1) return;
    for(int i = 0; i < p.nsteps; i++)
        port_.send(Command::setStep(static_cast<uint8_t>(i), p.steps[i].temp_c, p.steps[i].ramp_h, p.steps[i].hold_h));
    port_.send(Command::setStepCount(static_cast<uint8_t>(p.nsteps)));
    int64_t start_s = p.start_unix_ms / 1000;
    if(start_s <= 0 && clock_ && clock_->valid()) start_s = clock_->unixSeconds();   // no start date: now
    port_.send(Command::setBatch(static_cast<uint32_t>(start_s > 0 ? start_s : 0), p.name));
    bf_->imported(now_);   /* and log to it from now on */
}

void UiBridge::onStepCountThunk(int n, void * user)
{
    static_cast<UiBridge *>(user)->port_.send(Command::setStepCount(static_cast<uint8_t>(n)));
}

void UiBridge::poll(uint32_t now_ms)
{
    now_ = now_ms;
    port_.service(now_ms);
    HistoryChunk h;
    while(port_.pollHistory(h)) applyHistory(h);
    State s;
    if(port_.pollState(s)) apply(s);
    Settings st;
    if(port_.pollSettings(st)) apply(st);
    pushWifi();
    sendClock();
    tickBrewfather();
}

void UiBridge::tickBrewfather()
{
    if(!bf_) return;
    const bool online = wifi_ && wifi_->status().phase == net::SetupPhase::Connected;
    const int64_t unix_ms = clock_ && clock_->valid() ? clock_->unixSeconds() * 1000 : 0;
    bf_->tick(now_, online, unix_ms);
    const net::BrewfatherStatus & s = bf_->status();
    brew_bf_t u = {};
    switch(s.account) {
        case net::BfAccount::NotSetUp:    u.account = BREW_BF_NOT_SET_UP; break;
        case net::BfAccount::Checking:    u.account = BREW_BF_CHECKING; break;
        case net::BfAccount::Connected:   u.account = BREW_BF_CONNECTED; break;
        case net::BfAccount::Rejected:    u.account = BREW_BF_REJECTED; break;
        case net::BfAccount::Unreachable: u.account = BREW_BF_UNREACHABLE; break;
    }
    u.logging = s.stream_set;
    u.attached = s.attached;
    u.batch = s.batch;
    const int64_t age = bf_->lastLogAgeS();
    u.last_log_min = age < 0 ? -1 : static_cast<int>(age / 60);
    u.nbatches = s.nbatches;
    u.imported_index = -1;
    for(int i = 0; i < s.nbatches && i < BREW_BF_BATCHES_MAX; i++) {
        u.batches[i] = s.batches[i].label;
        if(s.logging_id[0] && std::strcmp(s.batches[i].id, s.logging_id) == 0) u.imported_index = i;
    }
    u.device = s.device;
    switch(s.log) {
        case net::BfLog::Off:         u.log = BREW_BF_LOG_OFF; break;
        case net::BfLog::NotAttached: u.log = BREW_BF_LOG_NOT_ATTACHED; break;
        case net::BfLog::Attached:    u.log = BREW_BF_LOG_ATTACHED; break;
        case net::BfLog::Elsewhere:   u.log = BREW_BF_LOG_ELSEWHERE; break;
    }
    u.elsewhere = s.elsewhere;
    const net::BfPreview & p = s.preview;
    switch(p.state) {
        case net::BfPreview::State::None:    u.preview = BREW_BF_PREVIEW_NONE; break;
        case net::BfPreview::State::Loading: u.preview = BREW_BF_PREVIEW_LOADING; break;
        case net::BfPreview::State::Ready:   u.preview = BREW_BF_PREVIEW_READY; break;
        case net::BfPreview::State::Failed:  u.preview = BREW_BF_PREVIEW_FAILED; break;
    }
    u.preview_label = p.label;
    u.preview_total = p.total_steps;
    u.preview_n = p.nsteps < BREW_BF_STEPS_MAX ? p.nsteps : BREW_BF_STEPS_MAX;
    for(int i = 0; i < u.preview_n; i++) {
        u.preview_steps[i].type = p.steps[i].type;
        u.preview_steps[i].temp_f = std::round(c_to_f(p.steps[i].temp_c) * 10.0f) / 10.0f;
        u.preview_steps[i].ramp_h = p.steps[i].ramp_h;
        u.preview_steps[i].hold_h = p.steps[i].hold_h;
    }
    /* where today falls: days since the batch's fermentation start, and which step runs now */
    u.today_day = -1;
    u.today_step = -1;
    if(p.start_unix_ms > 0 && unix_ms > 0) {
        const int64_t el = (unix_ms - p.start_unix_ms) / 1000;
        if(el >= 0) {
            u.today_day = static_cast<int>(el / 86400) + 1;
            int64_t t = el;
            for(int i = 0; i < p.nsteps; i++) {
                const int64_t len = (static_cast<int64_t>(p.steps[i].ramp_h) + p.steps[i].hold_h) * 3600;
                if(t < len) {
                    u.today_step = i;
                    break;
                }
                t -= len;
            }
        }
    }
    if(bf_pushed_ && s.rev == bf_rev_shown_ && u.account == bf_shown_.account && u.logging == bf_shown_.logging &&
       u.attached == bf_shown_.attached && u.last_log_min == bf_shown_.last_log_min && u.today_day == bf_shown_.today_day &&
       u.today_step == bf_shown_.today_step && u.log == bf_shown_.log && u.imported_index == bf_shown_.imported_index &&
       std::strcmp(s.batch, bf_batch_shown_) == 0)
        return;
    bf_rev_shown_ = s.rev;
    bf_shown_ = u;
    std::snprintf(bf_batch_shown_, sizeof(bf_batch_shown_), "%s", s.batch);
    bf_pushed_ = true;
    brew_ui_set_brewfather(&u);
}

void UiBridge::sendClock()
{
    if(!clock_ || (clock_sent_ && now_ - clock_sent_ms_ < kClockEveryMs) || !clock_->valid()) return;
    const int64_t t = clock_->unixSeconds();
    if(t <= 0 || t > 0xFFFFFFFFll) return;
    port_.send(Command::setClock(static_cast<uint32_t>(t)));
    clock_sent_ = true;
    clock_sent_ms_ = now_;
}

void UiBridge::pushWifi()
{
    if(!wifi_) return;
    const net::SetupStatus & w = wifi_->status();
    net::SetupStatus & v = wifi_shown_;
    if(wifi_pushed_ && w.phase == v.phase && w.error == v.error && std::strcmp(w.ssid, v.ssid) == 0 &&
       std::strcmp(w.ap_ssid, v.ap_ssid) == 0 && std::strcmp(w.ap_password, v.ap_password) == 0 &&
       std::strcmp(w.address, v.address) == 0)
        return;   // the countdown and phone count aren't shown: no redraw for them
    v = w;
    wifi_pushed_ = true;
    brew_wifi_t u = {};
    switch(w.phase) {
        case net::SetupPhase::Off:         u.phase = BREW_WIFI_OFF; break;
        case net::SetupPhase::Hotspot:     u.phase = BREW_WIFI_SETUP; break;
        case net::SetupPhase::PhoneJoined: u.phase = BREW_WIFI_PHONE; break;
        case net::SetupPhase::Connecting:  u.phase = BREW_WIFI_CONNECTING; break;
        case net::SetupPhase::Connected:   u.phase = BREW_WIFI_CONNECTED; break;
        case net::SetupPhase::Offline:     u.phase = BREW_WIFI_OFFLINE; break;
    }
    switch(w.error) {
        case net::SetupError::None:          u.error = BREW_WIFI_ERR_NONE; break;
        case net::SetupError::WrongPassword: u.error = BREW_WIFI_ERR_PASSWORD; break;
        case net::SetupError::NotFound:      u.error = BREW_WIFI_ERR_NOT_FOUND; break;
        case net::SetupError::Failed:        u.error = BREW_WIFI_ERR_FAILED; break;
    }
    u.ssid = w.ssid;
    u.ap_ssid = w.ap_ssid;
    u.ap_password = w.ap_password;
    u.address = w.address;
    brew_ui_set_wifi(&u);
}

void UiBridge::apply(const Settings & s)
{
    Shown & v = shown_;
    brew_ui_set_demo(s.simulated);   /* no-op unless it changed */
    simulated_ = s.simulated;
    brew_schedule_t sc = {};
    sc.profile = s.profile == Profile::Schedule ? BREW_PROFILE_SCHEDULE : BREW_PROFILE_CONSTANT;
    sc.step = s.step == Settings::kDone ? BREW_STEP_DONE : s.step;
    sc.override_on = s.override_on;
    sc.nsteps = s.nsteps > BREW_STEPS_MAX ? BREW_STEPS_MAX : s.nsteps;
    for(int i = 0; i < sc.nsteps; i++) {
        sc.steps[i].temp_f = std::round(c_to_f(s.steps[i].temp_c) * 10.0f) / 10.0f;   /* centi-C round trip */
        sc.steps[i].ramp_h = s.steps[i].ramp_h;
        sc.steps[i].hold_h = s.steps[i].hold_h;
    }
    if(!v.sched_shown || std::memcmp(&sc, &v.sched, sizeof(sc)) != 0) {
        v.sched_shown = true;
        v.sched = sc;
        brew_ui_set_schedule(&sc);
    }
    if(v.tuned && s.heat_diff_c == v.heat_diff_c && s.cool_diff_c == v.cool_diff_c && s.heat_delay_s == v.heat_delay_s &&
       s.cool_delay_s == v.cool_delay_s && s.crash_default_c == v.crash_c)
        return;   // re-sent unchanged (it is, every few seconds)
    v.tuned = true;
    v.heat_diff_c = s.heat_diff_c;
    v.cool_diff_c = s.cool_diff_c;
    v.heat_delay_s = s.heat_delay_s;
    v.cool_delay_s = s.cool_delay_s;
    v.crash_c = s.crash_default_c;
    brew_ui_set_tuning(s.heat_diff_c * 9.0f / 5.0f, s.cool_diff_c * 9.0f / 5.0f, s.heat_delay_s, s.cool_delay_s,
                       c_to_f(s.crash_default_c));
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
    if(bf_) {   /* the latest reading, for the next 15-minute post; a simulated (demo) one never goes to a real batch */
        const char * st = s.paused ? "off" : s.action == Action::Heating ? "heating" : s.action == Action::Cooling ? "cooling" : "idle";
        bf_->setReading(s.temp_valid && !simulated_, s.temp_c, s.setpoint_c[static_cast<int>(s.mode)], st);
    }
    if(s.temp_valid) {
        const float tf = c_to_f(s.temp_c);
        if(!v.temp_set || std::fabs(tf - v.temp_f) > 0.001f) {
            v.temp_f = tf;
            v.temp_set = true;
            brew_ui_set_temp(tf);
        }
    }
    else if(all || v.temp_set) {   /* no probe, or it stopped answering: never show a stale number */
        v.temp_set = false;
        brew_ui_set_no_reading();
    }
    if(all || s.action != v.action) {
        v.action = s.action;
        brew_ui_set_action(to_ui(s.action));
    }
    if(all || s.wanted != v.wanted || s.wait_s != v.wait_s) {
        v.wanted = s.wanted;
        v.wait_s = s.wait_s;
        brew_ui_set_pending(to_ui(s.wanted), s.wait_s);
    }
}

void UiBridge::applyHistory(const HistoryChunk & h)
{
    float f[HistoryChunk::kMax];
    for(int i = 0; i < h.n; i++) f[i] = c_to_f(h.temps_c[i]);
    if(h.reset) {
        brew_ui_set_history(f, h.n, static_cast<int>(h.ms_per_sample));
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
