#include "harness.h"

#include "brew_layout.h"
#include "brew_ui.h"
#include "lvgl.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace brew::sim {

namespace {
Harness * g_harness = nullptr;

constexpr size_t kLogMax = 300;

const char * action_name(Action a)
{
    switch(a) {
        case Action::Heating: return "heating";
        case Action::Cooling: return "cooling";
        default:              return "idle";
    }
}

/* JSON number that is never NaN/inf (JSON can't carry those). */
double num(double v) { return std::isfinite(v) ? v : 0.0; }
}  // namespace

/* ------------------------------------------------------------------ ports */

/** Display side's UiPort: forwards to LocalBus or the remote proxy, and logs every Command. */
class Harness::UiFront final : public UiPort {
public:
    explicit UiFront(Harness & h) : h_(h) {}

    void send(const Command & c) override
    {
        switch(c.kind) {
            case Command::Kind::SetSetpoint:
                h_.log("UI -> controller: %s target %.1f\xC2\xB0""F (%.2f\xC2\xB0""C)",
                       c.mode == Mode::Crash ? "crash" : "ferment", c_to_f(c.setpoint_c), c.setpoint_c);
                break;
            case Command::Kind::SetMode:
                h_.log("UI -> controller: mode %s%s", c.mode == Mode::Crash ? "crash" : "ferment",
                       c.paused ? ", paused" : "");
                break;
            case Command::Kind::SetUnits:
                h_.log("UI -> controller: units %s", c.celsius ? "\xC2\xB0""C" : "\xC2\xB0""F");
                break;
            case Command::Kind::RequestSync:
                h_.log("UI -> controller: request full sync");
                break;
            case Command::Kind::SetClock:   /* every minute: only the first one is news */
                if(!clock_logged_) h_.log("UI -> controller: internet time");
                clock_logged_ = true;
                break;
            case Command::Kind::StartDayCount:
                h_.log("UI -> controller: day count starts over");
                break;
            case Command::Kind::SetProfile:
                h_.log("UI -> controller: profile %s", c.value == 1 ? "schedule" : "constant");
                break;
            case Command::Kind::SetStep:
                h_.log("UI -> controller: step %d = %.1f\xC2\xB0""F, ramp %u h, hold %u h", c.step + 1, c_to_f(c.setpoint_c),
                       static_cast<unsigned>(c.ramp_h), static_cast<unsigned>(c.hold_h));
                break;
            case Command::Kind::SetStepCount:
                h_.log("UI -> controller: %ld steps", static_cast<long>(c.value));
                break;
            case Command::Kind::SetTuning: {
                static const char * const what[] = { "heat differential", "cool differential", "heat restart delay",
                                                     "cool restart delay", "cold crash" };
                const int k = static_cast<int>(c.tuning);
                h_.log("UI -> controller: %s = %ld", k < 5 ? what[k] : "?", static_cast<long>(c.value));
                break;
            }
        }
        port().send(c);
    }
    bool pollState(State & s) override { return port().pollState(s); }
    bool pollHistory(HistoryChunk & h) override { return port().pollHistory(h); }
    bool pollSettings(Settings & s) override { return port().pollSettings(s); }
    void service(uint32_t now) override { port().service(now); }

private:
    UiPort & port() { return h_.split_ ? static_cast<UiPort &>(h_.remote_ctl_) : static_cast<UiPort &>(h_.bus_); }
    Harness & h_;
    bool clock_logged_ = false;
};

/** Controller side's ControlPort: LocalBus or the remote proxy. */
class Harness::CtlFront final : public ControlPort {
public:
    explicit CtlFront(Harness & h) : h_(h) {}
    bool pollCommand(Command & c) override { return port().pollCommand(c); }
    void publish(const State & s) override { port().publish(s); }
    bool publish(const HistoryChunk & h) override { return port().publish(h); }
    void publish(const Settings & s) override { port().publish(s); }
    void service(uint32_t now) override { port().service(now); }

private:
    ControlPort & port()
    {
        return h_.split_ ? static_cast<ControlPort &>(h_.remote_ui_) : static_cast<ControlPort &>(h_.bus_);
    }
    Harness & h_;
};

/* ------------------------------------------------------------------ setup */

Harness::Harness(int size)
    : size_(size),
      panel_(static_cast<uint16_t>(size), static_cast<uint16_t>(size)),
      knob_(&encoder_, &button_, KnobInput::Config{}),
      buttons_(&keys_[0], &keys_[1], &keys_[2], ButtonInput::Config{}),
      plant_(plant_cfg_)
{
    g_harness = this;
}

Harness::~Harness()
{
    if(g_harness == this) g_harness = nullptr;
}

uint32_t Harness::lvTick()
{
    return g_harness ? g_harness->ui_ms_ : 0;
}

void Harness::start(uint32_t ui_now)
{
    ui_ms_ = ui_now;

    lv_init();
    lv_tick_set_cb(lvTick);
    const size_t bytes = static_cast<size_t>(size_) * panel_.drawBufferLines() * 2;
    buf1_.assign(bytes, 0);
    buf2_.assign(bytes, 0);
    lvgl_.begin(panel_, buf1_.data(), buf2_.data(), bytes, 16);
    lvgl_.addTouch();

    /* Controller first, so the UI's sync request has someone to answer it. */
    ctl_front_ = std::make_unique<CtlFront>(*this);
    buildController();

    knob_.begin();
    ui_front_ = std::make_unique<UiFront>(*this);
    bridge_ = std::make_unique<app::UiBridge>(*ui_front_);
    bridge_->create(lv_screen_active());

    /* The display board's WiFi: setup opens by itself when nothing is saved. Its settings share
     * the one NVS with the controller, as on a single-board CrowPanel. */
    /* The bench starts as a device that was set up before (so the dial comes up as usual);
     * "Forget WiFi", a factory reset or index.html?wifi=new shows a first start. */
    settings_.setStr("wifi_ssid", "Hartmann-5G");
    settings_.setStr("wifi_pass", "hops2026");
    wifi_setup_ = std::make_unique<net::WifiSetup>(wifi_, &settings_, net::WifiSetup::Config{});
    portal_ = std::make_unique<net::SetupPortal>(*wifi_setup_);
    wifi_setup_->begin(ui_now);
    bridge_->attachWifi(wifi_setup_.get());
    bridge_->attachClock(&clock_);
    /* Brewfather: not set up until the page (bd_http POST /api/brewfather) gives it credentials */
    brewfather_ = std::make_unique<net::Brewfather>(http_client_, &settings_);
    brewfather_->begin(wifi_setup_->status().ap_ssid, ui_now);
    portal_->attachBrewfather(brewfather_.get());
    bridge_->attachBrewfather(brewfather_.get());
    watchWifi();

    started_ = true;
    log("Harness ready: %dx%d screen, controller on the same board", size_, size_);
}

void Harness::buildController()
{
    /* A real reboot resets the expander, so both outputs drop before the new firmware runs. */
    plant_.heat(&heat_).write(false);
    plant_.cool(&cool_).write(false);
    ctl_.reset();
    guard_.reset();

    const float heat_c = setup_.heat_diff_f * 5.0f / 9.0f, cool_c = setup_.cool_diff_f * 5.0f / 9.0f;
    if(setup_.law == 1) law_ = std::make_unique<HysteresisLaw>(heat_c, cool_c);
    else law_ = std::make_unique<DeadbandLaw>(heat_c, cool_c);

    OutputGuard::Config g;
    g.heat.min_on_ms = setup_.heat_min_on_s * 1000u;
    g.heat.min_off_ms = setup_.heat_min_off_s * 1000u;
    g.cool.min_on_ms = setup_.cool_min_on_s * 1000u;
    g.cool.min_off_ms = setup_.cool_min_off_s * 1000u;
    g.changeover_ms = setup_.changeover_s * 1000u;
    guard_ = std::make_unique<OutputGuard>(&plant_.heat(&heat_), &plant_.cool(&cool_), g);

    Controller::Config c;
    c.history_period_ms = setup_.history_period_ms;
    c.heat_diff_c = heat_c;   /* the bench's setup is the controller's default; the dial can change it */
    c.cool_diff_c = cool_c;
    c.heat_delay_s = static_cast<uint16_t>(setup_.heat_min_off_s);
    c.cool_delay_s = static_cast<uint16_t>(setup_.cool_min_off_s);
    ctl_ = std::make_unique<Controller>(*ctl_front_, probe_, *law_, *guard_, &settings_, c);
    boots_++;
    last_action_ = Action::Idle;
}

/* ------------------------------------------------------------------ time */

void Harness::frame(uint32_t ui_now)
{
    if(!started_ || ui_now <= ui_ms_) return;
    if(ui_now - ui_ms_ > 250) ui_ms_ = ui_now - 250;   // tab was in the background: don't catch up

    while(ui_ms_ < ui_now) {
        const uint32_t step = std::min<uint32_t>(5, ui_now - ui_ms_);
        ui_ms_ += step;
        while(!button_q_.empty() && button_q_.front().at <= ui_ms_) {
            button_.set(button_q_.front().down);
            button_q_.pop_front();
        }
        /* Same order as the firmware: input events, then controller -> UI, then render. */
        knob_.poll(ui_ms_, [&](const InputEvent & e) { bridge_->handle(e); });
        buttons_.poll(ui_ms_, [&](const InputEvent & e) { bridge_->handle(e); });
        advanceSim(static_cast<float>(step) * speed_);
    }
    bridge_->poll(ui_ms_);
    wifi_setup_->tick(ui_ms_);
    watchWifi();
    if(ui_loop_) ui_loop_(ui_loop_ctx_, ui_ms_);
    lv_timer_handler();
}

/* ------------------------------------------------------------------ WiFi */

void Harness::http(const net::HttpRequest & req, net::HttpResponse & res)
{
    portal_->handle(req, ui_ms_, res);
    wifi_setup_->tick(ui_ms_);
    watchWifi();
}

void Harness::setWifiPhone(bool on)
{
    wifi_.phone = on && wifi_.apOn();   // only possible while the setup network is up
}

void Harness::setWifiRouter(bool up)
{
    if(up == wifi_.router_up) return;
    wifi_.router_up = up;
    log(up ? "WiFi: router on" : "WiFi: router off");
}

void Harness::wifiStartSetup() { wifi_setup_->startSetup(ui_ms_); watchWifi(); }
void Harness::wifiCancelSetup() { wifi_setup_->cancelSetup(ui_ms_); watchWifi(); }
void Harness::wifiForget()
{
    wifi_setup_->forget(ui_ms_);
    log("WiFi: saved network forgotten");
    watchWifi();
}

void Harness::watchWifi()
{
    const net::SetupStatus & w = wifi_setup_->status();
    /* a new password means it was reopened, even if it never showed as down in between */
    if(wifi_.apOn() != last_ap_ || (wifi_.apOn() && last_ap_pw_ != w.ap_password)) {
        last_ap_ = wifi_.apOn();
        last_ap_pw_ = w.ap_password;
        if(last_ap_) log("WiFi: setup network %s is up (password %s)", w.ap_ssid, w.ap_password);
        else log("WiFi: setup network closed");
    }
    if(w.clients != last_clients_) {
        last_clients_ = w.clients;
        log(w.clients ? "WiFi: a phone joined the setup network" : "WiFi: the phone left the setup network");
    }
    if(w.error != last_wifi_error_) {
        last_wifi_error_ = w.error;
        if(w.error == net::SetupError::WrongPassword) log("WiFi: wrong password for %s", w.ssid);
        else if(w.error == net::SetupError::NotFound) log("WiFi: %s not found", w.ssid);
        else if(w.error == net::SetupError::Failed) log("WiFi: couldn't connect to %s", w.ssid);
    }
    if(w.phase != last_wifi_phase_) {
        last_wifi_phase_ = w.phase;
        switch(w.phase) {
            case net::SetupPhase::Connecting: log("WiFi: connecting to %s", w.ssid); break;
            case net::SetupPhase::Connected:  log("WiFi: connected to %s", w.ssid); break;
            case net::SetupPhase::Offline:    log("WiFi: not connected; retrying %s", w.ssid); break;
            case net::SetupPhase::Off:        log("WiFi: off (no network saved)"); break;
            default: break;
        }
    }
}

void Harness::advanceSim(float ms)
{
    sim_frac_ += ms;
    const uint32_t chunk = speed_ > 60.0f ? 250 : 50;   // controller tick granularity in sim time
    while(sim_frac_ >= 1.0f) {
        const uint32_t s = std::min<uint32_t>(chunk, static_cast<uint32_t>(sim_frac_));
        sim_ms_ += s;
        sim_frac_ -= static_cast<float>(s);
        ctl_->tick(sim_ms_);
        watchController();
    }
}

void Harness::watchController()
{
    const State & s = ctl_->state();
    if(s.temp_valid != last_valid_) {
        last_valid_ = s.temp_valid;
        log(s.temp_valid ? "Probe reading valid again" : "Probe reading INVALID: controller turns every output off");
    }
    if(s.action != last_action_) {
        last_action_ = s.action;
        const float sp = s.setpoint_c[static_cast<int>(s.mode)];
        log("Controller: %s (probe %.2f\xC2\xB0""F, target %.2f\xC2\xB0""F)", action_name(s.action), c_to_f(s.temp_c),
            c_to_f(sp));
    }
}

/* ------------------------------------------------------------------ inputs */

void Harness::turn(int detents)
{
    encoder_.add(detents * KnobInput::Config{}.counts_per_detent);
}

void Harness::encoderCounts(int counts)
{
    encoder_.add(counts);
}

void Harness::button(bool down)
{
    /* Keep the queue in time order (a click's release may already be scheduled). */
    Scheduled s{ ui_ms_, down };
    auto it = std::upper_bound(button_q_.begin(), button_q_.end(), s,
                               [](const Scheduled & a, const Scheduled & b) { return a.at < b.at; });
    button_q_.insert(it, s);
}

void Harness::click()
{
    const uint32_t at = std::max(ui_ms_, button_q_.empty() ? 0u : button_q_.back().at + 40);
    button_q_.push_back({ at, true });
    button_q_.push_back({ at + 80, false });
}

void Harness::key(int which, bool down)
{
    if(which < 0 || which > 2) return;
    keys_[which].set(down);
}

void Harness::setGraphStyle(int style)
{
    brew_ui_set_graph_style(style == 1 ? BREW_GRAPH_MIST : BREW_GRAPH_HAIRLINE);
    log("Graph style: %s", brew_ui_graph_style() == BREW_GRAPH_MIST ? "mist" : "hairline");
}

void Harness::setSquare(bool square)
{
    brew_ui_set_layout(square ? &brew_layout_square : &brew_layout_round);
    log("Screen layout: %s", brew_ui_layout_name());
}

void Harness::touch(int x, int y, bool down)
{
    hal::TouchPoint p;
    p.x = static_cast<int16_t>(x);
    p.y = static_cast<int16_t>(y);
    p.pressed = down;
    touch_.step(p, [&](const InputEvent & e) { lvgl_.pushTouch(e); });
}

/* ------------------------------------------------------------------ beer / probe */

void Harness::setBeerC(float c)
{
    probe_.setBeer(c);
    log("Beer set to %.2f\xC2\xB0""F", c_to_f(c));
    if(ctl_) {
        ctl_->evaluateNow(sim_ms_);
        watchController();
    }
}

void Harness::setPhysics(bool on)
{
    probe_.setPhysics(on);
    log(on ? "Physics on: heating/cooling move the beer" : "Physics off: beer stays where you put it");
}

void Harness::setPlant(const FakePlant::Config & cfg)
{
    plant_cfg_ = cfg;
    plant_.reconfigure(cfg);
    log("Fermenter: %.1f gal, room %.1f F, loss %.2f W/K, heat %.0f W (lag %.0f s), cool %.0f W (lag %.0f s), yeast %.0f W",
        static_cast<double>(cfg.volume_l / 3.785f), static_cast<double>(c_to_f(cfg.ambient_c)),
        static_cast<double>(cfg.loss_w_per_k), static_cast<double>(cfg.heat_w), static_cast<double>(cfg.heat_lag_s),
        static_cast<double>(cfg.cool_w), static_cast<double>(cfg.cool_lag_s), static_cast<double>(cfg.yeast_w));
}

void Harness::setProbeFault(int f)
{
    if(f < 0 || f > 3) return;
    probe_.fault = static_cast<SimProbe::Fault>(f);
    static const char * names[] = { "none", "disconnected", "stuck", "noisy" };
    log("Probe fault: %s", names[f]);
}

void Harness::setNoiseC(float c) { probe_.noise_c = c; }
void Harness::setOffsetC(float c) { probe_.offset_c = c; }

/* ------------------------------------------------------------------ controller */

void Harness::setSpeed(float x)
{
    speed_ = std::max(0.0f, std::min(x, 3600.0f));
}

void Harness::rebootController()
{
    buildController();
    log("Controller rebooted (boot #%u): settings kept, history lost", static_cast<unsigned>(boots_));
}

void Harness::forgetTuning() { Controller::forgetTuning(settings_); }

void Harness::factoryReset()
{
    settings_.clear();
    wifi_setup_->forget(ui_ms_);
    buildController();
    log("Controller factory reset: settings wiped");
}

/* ------------------------------------------------------------------ link */

void Harness::setSplit(bool split)
{
    if(split == split_) return;
    split_ = split;
    wire_.clear();
    log(split ? "Split mode: display and controller now talk over the simulated cable"
              : "Same-board mode: display and controller share a LocalBus");
    ui_front_->send(Command::requestSync());
}

/* ------------------------------------------------------------------ outputs */

const uint8_t * Harness::rgba()
{
    if(rgba_at_ == panel_.flushes) return rgba_.data();
    rgba_at_ = panel_.flushes;
    const size_t n = static_cast<size_t>(size_) * size_;
    rgba_.resize(n * 4);
    const uint16_t * fb = panel_.framebuffer();
    for(size_t i = 0; i < n; i++) {
        const uint16_t p = fb[i];
        const uint8_t r = static_cast<uint8_t>((p >> 11) & 0x1F), g = static_cast<uint8_t>((p >> 5) & 0x3F),
                      b = static_cast<uint8_t>(p & 0x1F);
        rgba_[i * 4 + 0] = static_cast<uint8_t>((r << 3) | (r >> 2));
        rgba_[i * 4 + 1] = static_cast<uint8_t>((g << 2) | (g >> 4));
        rgba_[i * 4 + 2] = static_cast<uint8_t>((b << 3) | (b >> 2));
        rgba_[i * 4 + 3] = 255;
    }
    return rgba_.data();
}

std::string Harness::statusJson() const
{
    const State & s = ctl_->state();
    const hal::TempReading r = probe_.latest();
    const net::SetupStatus & w = wifi_setup_->status();
    char wifi[512];
    std::snprintf(wifi, sizeof(wifi),
                  "\"wifi\":{\"phase\":\"%s\",\"error\":\"%s\",\"ssid\":\"%s\",\"ap\":%s,\"ap_ssid\":\"%s\","
                  "\"ap_password\":\"%s\",\"clients\":%d,\"setup_left_s\":%u,\"phone\":%s,\"router\":%s,"
                  "\"joined\":\"%s\"}",
                  net::phaseName(w.phase), net::errorName(w.error), w.ssid, wifi_.apOn() ? "true" : "false",
                  w.ap_ssid, w.ap_password, w.clients, static_cast<unsigned>(w.setup_left_ms / 1000u),
                  wifi_.phone ? "true" : "false", wifi_.router_up ? "true" : "false", wifi_.joined().c_str());
    char tune[200];
    std::snprintf(tune, sizeof(tune),
                  "\"tuning\":{\"heat_diff_c\":%.3f,\"cool_diff_c\":%.3f,\"heat_delay_s\":%u,\"cool_delay_s\":%u,"
                  "\"crash_c\":%.2f}",
                  num(ctl_->settings().heat_diff_c), num(ctl_->settings().cool_diff_c),
                  static_cast<unsigned>(ctl_->settings().heat_delay_s), static_cast<unsigned>(ctl_->settings().cool_delay_s),
                  num(ctl_->settings().crash_default_c));
    char ui_status[48];
    brew_ui_status_text(ui_status, sizeof(ui_status));
    char b[4096];
    std::snprintf(
        b, sizeof(b),
        "{\"ui_ms\":%u,\"sim_ms\":%u,\"speed\":%g,\"split\":%s,\"boots\":%u,\"commits\":%u,"
        "\"beer_c\":%.4f,\"reading\":{\"valid\":%s,\"c\":%.4f},"
        "\"probe\":{\"fault\":%d,\"physics\":%s,\"noise_c\":%g,\"offset_c\":%g},"
        "\"plant\":{\"ambient_c\":%g,\"volume_l\":%g,\"loss_w_per_k\":%g,\"heat_w\":%g,\"cool_w\":%g,"
        "\"heat_lag_s\":%g,\"cool_lag_s\":%g,\"yeast_w\":%g,\"heat_power_w\":%.2f,\"cool_power_w\":%.2f},"
        "\"state\":{\"temp_c\":%.4f,\"valid\":%s,\"sp\":[%.4f,%.4f],\"mode\":%d,\"paused\":%s,\"action\":%d,"
        "\"celsius\":%s,\"batch\":\"%s\",\"day\":%u,\"clock_s\":%u,\"clock_unix\":%s},"
        "\"relays\":{\"heat\":{\"on\":%s,\"switches\":%u,\"on_ms\":%llu,\"in_state_ms\":%u,\"wait_ms\":%u},"
        "\"cool\":{\"on\":%s,\"switches\":%u,\"on_ms\":%llu,\"in_state_ms\":%u,\"wait_ms\":%u}},"
        "\"link\":{\"unplugged\":%s,\"drop\":%g,\"corrupt\":%g,\"bytes_to_ctl\":%llu,\"bytes_to_ui\":%llu,"
        "\"bytes_lost\":%llu,\"bytes_corrupted\":%llu,\"bad_frames_display\":%u,\"bad_frames_controller\":%u},"
        "\"setup\":{\"law\":%d,\"heat_diff_f\":%g,\"cool_diff_f\":%g,\"heat_min_on_s\":%u,\"heat_min_off_s\":%u,"
        "\"cool_min_on_s\":%u,\"cool_min_off_s\":%u,\"changeover_s\":%u,\"history_period_ms\":%u},"
        "\"knob\":{\"button\":%s},\"layout\":\"%s\",\"graph_style\":\"%s\","
        "\"pending\":{\"wanted\":%d,\"wait_s\":%u},\"ui_status\":\"%s\",%s,%s}",
        ui_ms_, sim_ms_, static_cast<double>(speed_), split_ ? "true" : "false", static_cast<unsigned>(boots_),
        static_cast<unsigned>(settings_.commits), num(plant_.actual()), r.valid ? "true" : "false", num(r.celsius),
        static_cast<int>(probe_.fault), probe_.physics ? "true" : "false", static_cast<double>(probe_.noise_c),
        static_cast<double>(probe_.offset_c), static_cast<double>(plant_cfg_.ambient_c),
        static_cast<double>(plant_cfg_.volume_l), static_cast<double>(plant_cfg_.loss_w_per_k),
        static_cast<double>(plant_cfg_.heat_w), static_cast<double>(plant_cfg_.cool_w),
        static_cast<double>(plant_cfg_.heat_lag_s), static_cast<double>(plant_cfg_.cool_lag_s),
        static_cast<double>(plant_cfg_.yeast_w), static_cast<double>(plant_.heatPowerW()),
        static_cast<double>(plant_.coolPowerW()), num(s.temp_c), s.temp_valid ? "true" : "false",
        num(s.setpoint_c[0]), num(s.setpoint_c[1]), static_cast<int>(s.mode), s.paused ? "true" : "false",
        static_cast<int>(s.action), s.celsius ? "true" : "false", s.batch, static_cast<unsigned>(s.day),
        static_cast<unsigned>(ctl_->clockS()), ctl_->clockIsUnix() ? "true" : "false",
        heat_.on() ? "true" : "false", static_cast<unsigned>(heat_.switches()),
        static_cast<unsigned long long>(heat_.onMs()), static_cast<unsigned>(heat_.inStateMs()),
        static_cast<unsigned>(heat_.on() ? 0u : guard_->blockedFor(Action::Heating, sim_ms_)),
        cool_.on() ? "true" : "false", static_cast<unsigned>(cool_.switches()),
        static_cast<unsigned long long>(cool_.onMs()), static_cast<unsigned>(cool_.inStateMs()),
        static_cast<unsigned>(cool_.on() ? 0u : guard_->blockedFor(Action::Cooling, sim_ms_)),
        wire_.unplugged ? "true" : "false", static_cast<double>(wire_.drop_rate),
        static_cast<double>(wire_.corrupt_rate), static_cast<unsigned long long>(wire_.bytes[0]),
        static_cast<unsigned long long>(wire_.bytes[1]), static_cast<unsigned long long>(wire_.dropped),
        static_cast<unsigned long long>(wire_.corrupted), static_cast<unsigned>(display_link_.droppedFrames()),
        static_cast<unsigned>(controller_link_.droppedFrames()), setup_.law, static_cast<double>(setup_.heat_diff_f),
        static_cast<double>(setup_.cool_diff_f), static_cast<unsigned>(setup_.heat_min_on_s),
        static_cast<unsigned>(setup_.heat_min_off_s), static_cast<unsigned>(setup_.cool_min_on_s),
        static_cast<unsigned>(setup_.cool_min_off_s),
        static_cast<unsigned>(setup_.changeover_s), static_cast<unsigned>(setup_.history_period_ms),
        button_.down() ? "true" : "false", brew_ui_layout_name(),
        brew_ui_graph_style() == BREW_GRAPH_MIST ? "mist" : "hairline", static_cast<int>(s.wanted),
        static_cast<unsigned>(s.wait_s), ui_status, wifi, tune);
    return b;
}

void Harness::log(const char * fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    const uint32_t t = sim_ms_ / 1000;
    char line[300];
    std::snprintf(line, sizeof(line), "[%02u:%02u:%02u] %s", t / 3600, (t / 60) % 60, t % 60, msg);
    log_.emplace_back(line);
    if(log_.size() > kLogMax) log_.pop_front();
}

bool Harness::nextLog(std::string & line)
{
    if(log_.empty()) return false;
    line = std::move(log_.front());
    log_.pop_front();
    return true;
}

}  // namespace brew::sim
