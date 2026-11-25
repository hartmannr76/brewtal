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
constexpr const char * kKeyProfile = "profile";
constexpr const char * kKeySchedN = "sch_n";
constexpr const char * kKeySchedStart = "sch_start";     // day-count clock, seconds
constexpr const char * kKeySchedFrom = "sch_from_cc";
constexpr const char * kKeySchedOvr = "sch_ovr_cc";      // the override target, centi-C
constexpr const char * kKeySchedOvrStep = "sch_ovr_step"; // ...and its step (-1 = none)
constexpr const char * kKeyClock = "clk_s";      // day-count clock, seconds
constexpr const char * kKeyDayStart = "day0_s";  // clock value when Day 1 began
constexpr const char * kKeyClockUnix = "clk_unix"; // 1: the clock values are Unix time
/* Clock values are saved as the bits of a uint32 in an i32 (Unix time passes 2^31 in 2038). */
int32_t clockBits(uint32_t s) { return static_cast<int32_t>(s); }
constexpr uint32_t kMinUnix = 1735689600u;   // 2025-01-01: anything earlier isn't a real time
constexpr uint32_t kClockSaveJumpS = 60;     // a SetClock that moves the clock this far is saved
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
    set_.nsteps = cfg_.nsteps > Settings::kMaxSteps ? Settings::kMaxSteps : cfg_.nsteps;
    for(int i = 0; i < set_.nsteps; i++) set_.steps[i] = cfg_.steps[i];
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
        /* the schedule: steps as typed keys (sch_t0 / sch_r0 / sch_h0 ...), then where it is */
        if(store_->getI32(kKeySchedN, v) && v >= 0 && v <= Settings::kMaxSteps) {
            set_.nsteps = static_cast<uint8_t>(v);
            for(int i = 0; i < set_.nsteps; i++) {
                char k[16];
                std::snprintf(k, sizeof(k), "sch_t%d", i);
                if(store_->getI32(k, v)) set_.steps[i].temp_c = static_cast<float>(v) / 100.0f;
                std::snprintf(k, sizeof(k), "sch_r%d", i);
                if(store_->getI32(k, v) && v >= 0) set_.steps[i].ramp_h = static_cast<uint16_t>(v);
                std::snprintf(k, sizeof(k), "sch_h%d", i);
                if(store_->getI32(k, v) && v >= 0) set_.steps[i].hold_h = static_cast<uint16_t>(v);
            }
        }
        if(store_->getI32(kKeyProfile, v) && v >= 0 && v < kProfileCount) set_.profile = static_cast<Profile>(v);
        if(store_->getI32(kKeySchedStart, v)) sched_start_s_ = static_cast<uint32_t>(v);
        if(store_->getI32(kKeySchedFrom, v)) sched_from_c_ = static_cast<float>(v) / 100.0f;
        int32_t os = -1;
        if(store_->getI32(kKeySchedOvrStep, os) && os >= 0 && store_->getI32(kKeySchedOvr, v)) {
            set_.override_on = true;
            set_.step = static_cast<uint8_t>(os);
            override_c_ = static_cast<float>(v) / 100.0f;
        }
        if(store_->getI32(kKeyClockUnix, v)) clk_unix_ = v == 1;
        if(store_->getI32(kKeyClock, v)) clk_s_ = static_cast<uint32_t>(v);
        if(store_->getI32(kKeyDayStart, v) && static_cast<uint32_t>(v) <= clk_s_)
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

/* Step 1 begins now, ramping from today's Ferment target. */
void Controller::startSchedule()
{
    sched_start_s_ = clk_s_;
    sched_from_c_ = st_.setpoint_c[static_cast<int>(Mode::Ferment)];
    set_.step = 0;
    set_.override_on = false;
    if(!store_) return;
    store_->setI32(kKeySchedStart, clockBits(sched_start_s_));
    store_->setI32(kKeySchedFrom, static_cast<int32_t>(std::lround(sched_from_c_ * 100.0f)));
    store_->setI32(kKeySchedOvrStep, -1);
    store_->commit();
}

void Controller::saveSchedule()
{
    if(!store_) return;
    store_->setI32(kKeySchedN, set_.nsteps);
    for(int i = 0; i < set_.nsteps; i++) {
        char k[16];
        std::snprintf(k, sizeof(k), "sch_t%d", i);
        store_->setI32(k, static_cast<int32_t>(std::lround(set_.steps[i].temp_c * 100.0f)));
        std::snprintf(k, sizeof(k), "sch_r%d", i);
        store_->setI32(k, set_.steps[i].ramp_h);
        std::snprintf(k, sizeof(k), "sch_h%d", i);
        store_->setI32(k, set_.steps[i].hold_h);
    }
    store_->commit();
}

/* Where the schedule is now: each step ramps from the previous step's temperature (step 1 from
 * the Ferment target when it started) over ramp_h, then holds for hold_h. After the last step,
 * its temperature holds. A dial override holds until the step changes. */
float Controller::scheduleTarget()
{
    uint32_t t = clk_s_ - sched_start_s_;
    float prev = sched_from_c_;
    uint8_t step = Settings::kDone;
    float target = set_.steps[set_.nsteps - 1].temp_c;
    for(uint8_t i = 0; i < set_.nsteps; i++) {
        const ScheduleStep & s = set_.steps[i];
        const uint32_t ramp = static_cast<uint32_t>(s.ramp_h) * 3600u, hold = static_cast<uint32_t>(s.hold_h) * 3600u;
        if(t < ramp + hold) {
            step = i;
            target = t < ramp ? prev + (s.temp_c - prev) * static_cast<float>(t) / static_cast<float>(ramp) : s.temp_c;
            break;
        }
        t -= ramp + hold;
        prev = s.temp_c;
    }
    if(step != set_.step) {   /* a new step: any override ends with the old one */
        set_.step = step;
        if(set_.override_on) {
            set_.override_on = false;
            if(store_) {
                store_->setI32(kKeySchedOvrStep, -1);
                store_->commit();
            }
        }
        publishSettings(last_eval_);
    }
    return set_.override_on ? override_c_ : target;
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
    store_->setI32(kKeyClock, clockBits(clk_s_));
    store_->setI32(kKeyDayStart, clockBits(day0_s_));
    store_->commit();
}

/* The time from the internet. The first one turns the clock from seconds powered into Unix
 * time: Day 1's start and the schedule's start move by the same amount, so nothing visible
 * changes. After that it catches the clock up (time spent unplugged, drift). */
void Controller::setClock(uint32_t unix_s, uint32_t now)
{
    if(unix_s < kMinUnix) return;   // the sender's clock isn't set
    advanceClock(now);
    bool save = false;
    if(!clk_unix_) {
        const uint32_t shift = unix_s - clk_s_;
        day0_s_ += shift;
        sched_start_s_ += shift;
        clk_unix_ = true;
        save = true;
    }
    else {   /* the clock never runs back past Day 1 or the schedule's start */
        if(unix_s < day0_s_) day0_s_ = unix_s, save = true;
        if(unix_s < sched_start_s_) sched_start_s_ = unix_s, save = true;
    }
    const uint32_t jump = unix_s > clk_s_ ? unix_s - clk_s_ : clk_s_ - unix_s;
    clk_s_ = unix_s;
    clk_ms_ = 0;
    updateDay();
    if(!save && jump < kClockSaveJumpS) return;   // small drift: the regular save keeps it
    if(store_) {
        store_->setI32(kKeyClockUnix, 1);
        store_->setI32(kKeySchedStart, clockBits(sched_start_s_));
    }
    saveClock();
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

    /* A running schedule sets the Ferment target (Cold crash ignores it). */
    if(scheduleRunning()) st_.setpoint_c[static_cast<int>(Mode::Ferment)] = scheduleTarget();

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
            if(c.mode == Mode::Ferment && scheduleRunning()) {
                /* the dial overrides the schedule until its next step (once it's done: from then on);
                 * the step itself is kept */
                set_.override_on = true;
                override_c_ = c.setpoint_c;
                if(store_) {
                    store_->setI32(kKeySchedOvr, static_cast<int32_t>(std::lround(override_c_ * 100.0f)));
                    store_->setI32(kKeySchedOvrStep, set_.step);
                }
                publishSettings(now);
            }
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
        case Command::Kind::SetProfile: {
            if(c.value < 0 || c.value >= kProfileCount) return;
            const Profile p = static_cast<Profile>(c.value);
            if(p == set_.profile) break;   /* picking the running profile again changes nothing */
            if(p == Profile::Schedule && set_.nsteps == 0) return;   /* nothing to run */
            set_.profile = p;
            if(p == Profile::Schedule) startSchedule();
            persist();   /* stopping a schedule keeps the target it had reached */
            if(store_) {
                store_->setI32(kKeyProfile, c.value);
                store_->commit();
            }
            evaluate(now);
            publishSettings(now);
            break;
        }
        case Command::Kind::SetStep: {
            if(c.step >= Settings::kMaxSteps || c.setpoint_c < -10.0f || c.setpoint_c > 40.0f) return;
            if(c.hold_h == 0 || c.hold_h > 24u * 90u || c.ramp_h > 24u * 30u) return;
            ScheduleStep & s = set_.steps[c.step];
            s.temp_c = c.setpoint_c;
            s.ramp_h = c.ramp_h;
            s.hold_h = c.hold_h;
            if(c.step >= set_.nsteps) set_.nsteps = static_cast<uint8_t>(c.step + 1);
            saveSchedule();
            evaluate(now);
            publishSettings(now);
            break;
        }
        case Command::Kind::SetStepCount: {
            /* Removing steps (adding is SetStep, which grows the list). An empty schedule can't run. */
            if(c.value < 0 || c.value > set_.nsteps) return;
            set_.nsteps = static_cast<uint8_t>(c.value);
            if(set_.nsteps == 0 && set_.profile == Profile::Schedule) {
                set_.profile = Profile::Constant;
                persist();
                if(store_) store_->setI32(kKeyProfile, static_cast<int32_t>(Profile::Constant));
            }
            saveSchedule();
            evaluate(now);
            publishSettings(now);
            break;
        }
        case Command::Kind::SetClock:
            setClock(static_cast<uint32_t>(c.value), now);
            break;
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
