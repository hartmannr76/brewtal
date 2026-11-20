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
        }
        port().send(c);
    }
    bool pollState(State & s) override { return port().pollState(s); }
    bool pollHistory(HistoryChunk & h) override { return port().pollHistory(h); }
    void service(uint32_t now) override { port().service(now); }

private:
    UiPort & port() { return h_.split_ ? static_cast<UiPort &>(h_.remote_ctl_) : static_cast<UiPort &>(h_.bus_); }
    Harness & h_;
};

/** Controller side's ControlPort: LocalBus or the remote proxy. */
class Harness::CtlFront final : public ControlPort {
public:
    explicit CtlFront(Harness & h) : h_(h) {}
    bool pollCommand(Command & c) override { return port().pollCommand(c); }
    void publish(const State & s) override { port().publish(s); }
    void publish(const HistoryChunk & h) override { port().publish(h); }
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
    c.history_minutes = setup_.history_minutes;
    c.day = 4;
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
    lv_timer_handler();
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

void Harness::factoryReset()
{
    settings_.clear();
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
    char ui_status[48];
    brew_ui_status_text(ui_status, sizeof(ui_status));
    char b[2048];
    std::snprintf(
        b, sizeof(b),
        "{\"ui_ms\":%u,\"sim_ms\":%u,\"speed\":%g,\"split\":%s,\"boots\":%u,\"commits\":%u,"
        "\"beer_c\":%.4f,\"reading\":{\"valid\":%s,\"c\":%.4f},"
        "\"probe\":{\"fault\":%d,\"physics\":%s,\"noise_c\":%g,\"offset_c\":%g},"
        "\"plant\":{\"ambient_c\":%g,\"volume_l\":%g,\"loss_w_per_k\":%g,\"heat_w\":%g,\"cool_w\":%g,"
        "\"heat_lag_s\":%g,\"cool_lag_s\":%g,\"yeast_w\":%g,\"heat_power_w\":%.2f,\"cool_power_w\":%.2f},"
        "\"state\":{\"temp_c\":%.4f,\"valid\":%s,\"sp\":[%.4f,%.4f],\"mode\":%d,\"paused\":%s,\"action\":%d,"
        "\"celsius\":%s,\"batch\":\"%s\",\"day\":%u},"
        "\"relays\":{\"heat\":{\"on\":%s,\"switches\":%u,\"on_ms\":%llu,\"in_state_ms\":%u,\"wait_ms\":%u},"
        "\"cool\":{\"on\":%s,\"switches\":%u,\"on_ms\":%llu,\"in_state_ms\":%u,\"wait_ms\":%u}},"
        "\"link\":{\"unplugged\":%s,\"drop\":%g,\"corrupt\":%g,\"bytes_to_ctl\":%llu,\"bytes_to_ui\":%llu,"
        "\"bytes_lost\":%llu,\"bytes_corrupted\":%llu,\"bad_frames_display\":%u,\"bad_frames_controller\":%u},"
        "\"setup\":{\"law\":%d,\"heat_diff_f\":%g,\"cool_diff_f\":%g,\"heat_min_on_s\":%u,\"heat_min_off_s\":%u,"
        "\"cool_min_on_s\":%u,\"cool_min_off_s\":%u,\"changeover_s\":%u,\"history_minutes\":%u},"
        "\"knob\":{\"button\":%s},\"layout\":\"%s\","
        "\"pending\":{\"wanted\":%d,\"wait_s\":%u},\"ui_status\":\"%s\"}",
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
        static_cast<unsigned>(setup_.changeover_s), static_cast<unsigned>(setup_.history_minutes),
        button_.down() ? "true" : "false", brew_ui_layout_name(), static_cast<int>(s.wanted),
        static_cast<unsigned>(s.wait_s), ui_status);
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
