#include "brew/controller.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace brew {

namespace {
/* Settings keys. Values are typed key/value pairs (never raw struct bytes), so adding or
 * removing a setting never invalidates the others. Keep keys <= 15 chars (NVS limit). */
constexpr const char * kKeySetpoint[kModeCount] = { "sp0_cc", "sp1_cc" };   // centi-degC
constexpr const char * kKeyMode = "mode";
constexpr const char * kKeyPaused = "paused";
constexpr const char * kKeyCelsius = "celsius";
constexpr const char * kKeyTuning[kTuningCount] = { "hdiff_cc", "cdiff_cc", "hdelay_s", "cdelay_s", "crash_cc" };
constexpr const char * kKeyClock = "clk_s";      // day-count clock, seconds
constexpr const char * kKeyDayStart = "day0_s";  // clock value when Day 1 began
/* Manual control: hold a target. (Fermentation profiles will name themselves here.) */
constexpr const char * kManual = "Manual";
constexpr uint32_t kDayS = 24u * 60u * 60u;
constexpr uint32_t kSettingsEveryMs = 5000;   // Settings re-sent this often (a display may have missed them)

int32_t to_centi(float c) { return static_cast<int32_t>(std::lround(c * 100.0f)); }
}  // namespace

Controller::Controller(ControlPort & port, hal::TempSensor & sensor, ControlLaw & law, OutputGuard & guard,
                       hal::SettingsStore * store, const Config & cfg)
    : port_(port), sensor_(sensor), law_(law), guard_(guard), store_(store), cfg_(cfg)
{
    for(int i = 0; i < kModeCount; i++) st_.setpoint_c[i] = cfg_.setpoint_c[i];
    set_.heat_diff_c = cfg_.heat_diff_c;
    set_.cool_diff_c = cfg_.cool_diff_c;
    set_.heat_delay_s = cfg_.heat_delay_s;
    set_.cool_delay_s = cfg_.cool_delay_s;
    set_.crash_default_c = cfg_.crash_default_c;
    std::snprintf(st_.batch, sizeof(st_.batch), "%s", kManual);
    st_.day = 1;
}

void Controller::begin(uint32_t now)
{
    sensor_.begin();
    if(store_) {
        int32_t v;
        for(int i = 0; i < kModeCount; i++)
            if(store_->getI32(kKeySetpoint[i], v)) st_.setpoint_c[i] = static_cast<float>(v) / 100.0f;
        if(store_->getI32(kKeyMode, v) && v >= 0 && v < kModeCount) st_.mode = static_cast<Mode>(v);
        if(store_->getI32(kKeyPaused, v)) st_.paused = v != 0;
        if(store_->getI32(kKeyCelsius, v)) st_.celsius = v != 0;
        for(int i = 0; i < kTuningCount; i++)   // -1 (or out of range) = not set: keep Config's
            if(store_->getI32(kKeyTuning[i], v) && v >= 0) setTuning(static_cast<Tuning>(i), v);
        if(store_->getI32(kKeyClock, v) && v >= 0) clk_s_ = static_cast<uint32_t>(v);
        if(store_->getI32(kKeyDayStart, v) && v >= 0 && static_cast<uint32_t>(v) <= clk_s_)
            day0_s_ = static_cast<uint32_t>(v);
        else {   // first start (or a bad value): Day 1 begins now
            day0_s_ = clk_s_;
            saveClock();
        }
    }
    clk_saved_s_ = clk_s_;
    clk_ms_ = 0;
    clk_last_ms_ = now;
    updateDay();
    applyTuning();
    publishSettings(now);
    last_eval_ = now - cfg_.period_ms;   // evaluate on the first tick
    last_hist_ = now;
    hist_sum_ = 0.0f;
    hist_cnt_ = 0;
    started_ = true;
    syncHistory();
}

void Controller::persist()
{
    if(!store_) return;
    for(int i = 0; i < kModeCount; i++) store_->setI32(kKeySetpoint[i], to_centi(st_.setpoint_c[i]));
    store_->setI32(kKeyMode, static_cast<int32_t>(st_.mode));
    store_->setI32(kKeyPaused, st_.paused ? 1 : 0);
    store_->setI32(kKeyCelsius, st_.celsius ? 1 : 0);
    store_->commit();
}

void Controller::forgetTuning(hal::SettingsStore & store)
{
    for(int i = 0; i < kTuningCount; i++) store.setI32(kKeyTuning[i], -1);
    store.commit();
}

/* Validates and stores one setting (centi-C or s). False if out of range. */
bool Controller::setTuning(Tuning t, int32_t v)
{
    switch(t) {
        case Tuning::HeatDiff:
        case Tuning::CoolDiff:
            if(v < 5 || v > 500) return false;   // 0.05..5 C
            (t == Tuning::HeatDiff ? set_.heat_diff_c : set_.cool_diff_c) = static_cast<float>(v) / 100.0f;
            return true;
        case Tuning::HeatDelay:
        case Tuning::CoolDelay:
            if(v < 0 || v > 3600) return false;  // up to an hour
            (t == Tuning::HeatDelay ? set_.heat_delay_s : set_.cool_delay_s) = static_cast<uint16_t>(v);
            return true;
        case Tuning::CrashDefault:
            if(v < -500 || v > 1500) return false;   // -5..15 C
            set_.crash_default_c = static_cast<float>(v) / 100.0f;
            return true;
    }
    return false;
}

void Controller::applyTuning()
{
    law_.setDiffs(set_.heat_diff_c, set_.cool_diff_c);
    guard_.setRestartDelays(static_cast<uint32_t>(set_.heat_delay_s) * 1000u, static_cast<uint32_t>(set_.cool_delay_s) * 1000u);
}

void Controller::publishSettings(uint32_t now)
{
    last_settings_ = now;
    port_.publish(set_);
}

void Controller::advanceClock(uint32_t now)
{
    clk_ms_ += now - clk_last_ms_;   // unsigned: fine across the 49-day millis wrap
    clk_last_ms_ = now;
    if(clk_ms_ >= 1000u) {
        clk_s_ += clk_ms_ / 1000u;
        clk_ms_ %= 1000u;
        updateDay();
    }
    if(cfg_.clock_save_s && clk_s_ - clk_saved_s_ >= cfg_.clock_save_s) saveClock();
}

void Controller::saveClock()
{
    clk_saved_s_ = clk_s_;
    if(!store_) return;
    store_->setI32(kKeyClock, static_cast<int32_t>(clk_s_ & 0x7FFFFFFFu));
    store_->setI32(kKeyDayStart, static_cast<int32_t>(day0_s_ & 0x7FFFFFFFu));
    store_->commit();
}

void Controller::updateDay()
{
    const uint32_t d = (clk_s_ - day0_s_) / kDayS + 1u;
    st_.day = static_cast<uint16_t>(d > 0xFFFFu ? 0xFFFFu : d);
}

void Controller::tick(uint32_t now)
{
    if(!started_) begin(now);
    port_.service(now);
    sensor_.update(now);
    advanceClock(now);

    Command c;
    while(port_.pollCommand(c)) handle(c, now);

    if(now - last_eval_ >= cfg_.period_ms) evaluate(now);
    if(now - last_settings_ >= kSettingsEveryMs) publishSettings(now);   /* lost ones are harmless */

    const uint32_t hist_ms = cfg_.history_period_ms;
    if(hist_ms && now - last_hist_ >= hist_ms) {
        last_hist_ = now;
        if(hist_cnt_ > 0) {
            const float mean = hist_sum_ / static_cast<float>(hist_cnt_);
            hist_sum_ = 0.0f;
            hist_cnt_ = 0;
            const bool dropped = hist_.size() == History::kCapacity;
            hist_.push(mean);
            if(syncing()) {
                /* the sync sends it when it reaches the end; keep its cursor on the same sample */
                if(dropped && sync_i_ > 0) sync_i_--;
            }
            else {
                HistoryChunk h;
                h.n = 1;
                h.temps_c[0] = mean;
                port_.publish(h);   // if the queue is full this one sample is lost; harmless
            }
        }
    }
    pumpSync();
}

void Controller::evaluateNow(uint32_t now)
{
    sensor_.update(now);
    evaluate(now);
}

void Controller::evaluate(uint32_t now)
{
    last_eval_ = now;
    const hal::TempReading r = sensor_.latest();
    st_.temp_valid = r.valid;
    if(r.valid) {
        st_.temp_c = r.celsius;
        hist_sum_ += r.celsius;
        hist_cnt_++;
    }

    /* Fail safe: no valid reading or paused -> everything off. */
    Action want = Action::Idle;
    if(!st_.paused && r.valid) want = law_.step(st_.temp_c, st_.setpoint_c[static_cast<int>(st_.mode)], now);
    st_.action = guard_.apply(want, now);
    st_.wanted = want;
    /* Held off (restart delay, min on, changeover)? Say for how long, so the display can
     * show "Cooling in 4 min" instead of "Holding" while the beer is above target. */
    const uint32_t wait_ms = want != st_.action ? guard_.blockedFor(want, now) : 0;
    const uint32_t wait_s = (wait_ms + 999u) / 1000u;
    st_.wait_s = static_cast<uint16_t>(wait_s > 0xFFFFu ? 0xFFFFu : wait_s);
    port_.publish(st_);
}

void Controller::handle(const Command & c, uint32_t now)
{
    switch(c.kind) {
        case Command::Kind::SetSetpoint: {
            const int m = static_cast<int>(c.mode);
            if(m < 0 || m >= kModeCount) return;
            st_.setpoint_c[m] = c.setpoint_c;
            persist();
            evaluate(now);
            break;
        }
        case Command::Kind::SetMode:
            if(static_cast<int>(c.mode) >= kModeCount) return;
            /* Cold crash starts at its setting; resuming a crash keeps what was adjusted since */
            if(c.mode == Mode::Crash && st_.mode != Mode::Crash) st_.setpoint_c[static_cast<int>(Mode::Crash)] = set_.crash_default_c;
            if(c.mode != st_.mode) law_.reset();
            st_.mode = c.mode;
            st_.paused = c.paused;
            persist();
            evaluate(now);
            break;
        case Command::Kind::SetUnits:
            st_.celsius = c.celsius;
            persist();
            port_.publish(st_);
            break;
        case Command::Kind::RequestSync:
            port_.publish(st_);
            publishSettings(now);
            syncHistory();
            break;
        case Command::Kind::SetTuning: {
            const int32_t cc = c.value;
            if(!setTuning(c.tuning, cc)) return;
            applyTuning();
            if(store_) {
                store_->setI32(kKeyTuning[static_cast<int>(c.tuning)], cc);
                store_->commit();
            }
            publishSettings(now);
            evaluate(now);
            break;
        }
        case Command::Kind::StartDayCount:
            advanceClock(now);
            day0_s_ = clk_s_;
            updateDay();
            saveClock();
            port_.publish(st_);
            break;
    }
}

void Controller::syncHistory()
{
    sync_i_ = 0;
    sync_reset_ = true;
    pumpSync();
}

void Controller::pumpSync()
{
    /* One chunk per tick: at the boards' 20 ms tick that's ~3 KB/s, well inside a 115200 baud
     * UART and the display's 1 KB receive buffer (a chunk is ~65 bytes on the wire). A full
     * window (18000 samples, 750 chunks) takes ~15 s; the graph fills in as it arrives. */
    if(syncing()) {
        HistoryChunk h;
        h.reset = sync_reset_;
        h.ms_per_sample = cfg_.history_period_ms;
        int i = sync_i_;
        while(i < hist_.size() && h.n < HistoryChunk::kMax) h.temps_c[h.n++] = hist_.at(i++);
        h.more = i < hist_.size();
        if(!port_.publish(h)) return;   // full: try again next tick
        sync_reset_ = false;
        sync_i_ = h.more ? i : -1;
    }
}

}  // namespace brew
