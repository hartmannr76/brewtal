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
constexpr const char * kKeyBatch = "batch";
constexpr const char * kKeyDay = "day";

int32_t to_centi(float c) { return static_cast<int32_t>(std::lround(c * 100.0f)); }
}  // namespace

Controller::Controller(ControlPort & port, hal::TempSensor & sensor, ControlLaw & law, OutputGuard & guard,
                       hal::SettingsStore * store, const Config & cfg)
    : port_(port), sensor_(sensor), law_(law), guard_(guard), store_(store), cfg_(cfg)
{
    for(int i = 0; i < kModeCount; i++) st_.setpoint_c[i] = cfg_.setpoint_c[i];
    setBatch(cfg_.batch, cfg_.day);
}

void Controller::setBatch(const char * name, uint16_t day)
{
    std::snprintf(st_.batch, sizeof(st_.batch), "%s", name ? name : "");
    st_.day = day;
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
        if(store_->getI32(kKeyDay, v) && v >= 0) st_.day = static_cast<uint16_t>(v);
        char b[State::kBatchLen];
        if(store_->getStr(kKeyBatch, b, sizeof(b))) setBatch(b, st_.day);
    }
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
    store_->setStr(kKeyBatch, st_.batch);
    store_->setI32(kKeyDay, st_.day);
    store_->commit();
}

void Controller::tick(uint32_t now)
{
    if(!started_) begin(now);
    port_.service(now);
    sensor_.update(now);

    Command c;
    while(port_.pollCommand(c)) handle(c, now);

    if(now - last_eval_ >= cfg_.period_ms) evaluate(now);

    const uint32_t hist_ms = static_cast<uint32_t>(cfg_.history_period_s) * 1000u;
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
            syncHistory();
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
     * UART and the display's 1 KB receive buffer (a chunk is ~60 bytes on the wire), and a
     * full 2 h window (150 chunks) still arrives in ~3 s. */
    if(syncing()) {
        HistoryChunk h;
        h.reset = sync_reset_;
        h.seconds_per_sample = cfg_.history_period_s;
        int i = sync_i_;
        while(i < hist_.size() && h.n < HistoryChunk::kMax) h.temps_c[h.n++] = hist_.at(i++);
        h.more = i < hist_.size();
        if(!port_.publish(h)) return;   // full: try again next tick
        sync_reset_ = false;
        sync_i_ = h.more ? i : -1;
    }
}

}  // namespace brew
