#include "brew/app/bench.h"

#include "brew/app/ui_bridge.h"
#include "brew/input.h"
#include "brew/network/wifi_setup.h"

#include <cstdio>

namespace brew::app {

/* ------------------------------------------------------------------ FrameMeter */

void FrameMeter::attach(lv_display_t * disp, MicrosFn micros)
{
    micros_ = micros;
    lv_display_add_event_cb(disp, onEvent, LV_EVENT_ALL, this);
}

bool FrameMeter::add(FrameWindow * w)
{
    for(FrameWindow *& slot : win_)
        if(!slot) {
            slot = w;
            return true;
        }
    return false;
}

void FrameMeter::remove(FrameWindow * w)
{
    for(FrameWindow *& slot : win_)
        if(slot == w) slot = nullptr;
}

void FrameMeter::onEvent(lv_event_t * e)
{
    auto * m = static_cast<FrameMeter *>(lv_event_get_user_data(e));
    switch(lv_event_get_code(e)) {
        case LV_EVENT_REFR_START:
            m->start_us_ = m->micros_ ? m->micros_() : 0;
            m->rendered_ = false;
            m->frame_px_ = 0;
            break;
        case LV_EVENT_FLUSH_START: {
            const auto * a = static_cast<const lv_area_t *>(lv_event_get_param(e));
            if(a) m->frame_px_ += static_cast<uint64_t>(lv_area_get_width(a)) * static_cast<uint64_t>(lv_area_get_height(a));
            break;
        }
        case LV_EVENT_RENDER_START:
            m->rendered_ = true;
            break;
        case LV_EVENT_INVALIDATE_AREA: {
            const auto * a = static_cast<const lv_area_t *>(lv_event_get_param(e));
            if(!a) break;
            const uint64_t px = static_cast<uint64_t>(lv_area_get_width(a)) * static_cast<uint64_t>(lv_area_get_height(a));
            for(FrameWindow * w : m->win_)
                if(w) w->inv_px += px;
            break;
        }
        case LV_EVENT_REFR_READY: {
            if(!m->rendered_) break;   // nothing was redrawn this cycle
            const uint32_t us = m->modelled_
                                    ? static_cast<uint32_t>(m->model_.base_us + m->model_.us_per_px * static_cast<double>(m->frame_px_))
                                    : static_cast<uint32_t>(m->micros_() - m->start_us_);
            m->frames_++;
            m->last_us_ = us;
            for(FrameWindow * w : m->win_) {
                if(!w) continue;
                w->frames++;
                w->sum_us += us;
                w->rendered_px += m->frame_px_;
                if(us > w->max_us) w->max_us = us;
            }
            break;
        }
        default:
            break;
    }
}

/* ------------------------------------------------------------------ Bench */

namespace {
const char * const kName[] = { "warmup", "idle", "turn_up", "settle", "turn_down", "settle", "menu", "full_redraw" };
constexpr uint32_t kStepMs = 300;    // knob detents, like a steady hand
constexpr int kSteps = 16;           // 8 F in half-degree steps
}  // namespace

Bench::Bench(UiBridge & ui, FrameMeter & meter, LogFn log, net::WifiSetup * wifi)
    : ui_(ui), meter_(meter), log_(log), wifi_(wifi)
{
    meter_.add(&win_);
}

Bench::~Bench() { meter_.remove(&win_); }

void Bench::begin(Phase p, uint32_t now)
{
    phase_ = p;
    t0_ = now;
    acts_ = 0;
    win_.clear();
}

void Bench::finish(uint32_t now)
{
    const bool measured = phase_ != Warmup && phase_ != SettleUp && phase_ != SettleDown;
    if(measured) {
        const uint32_t ms = now - t0_;
        char line[200];
        const uint32_t f = win_.frames;
        std::snprintf(line, sizeof(line),
                      "BENCH phase=%s ms=%u frames=%u fps=%.1f avg_ms=%.1f max_ms=%.1f px_per_frame=%u "
                      "rendered_px_per_frame=%u",
                      kName[phase_], static_cast<unsigned>(ms), static_cast<unsigned>(f), ms ? f * 1000.0 / ms : 0.0,
                      f ? win_.sum_us / 1000.0 / f : 0.0, win_.max_us / 1000.0,
                      static_cast<unsigned>(f ? win_.inv_px / f : 0), static_cast<unsigned>(f ? win_.rendered_px / f : 0));
        log_(line);
    }
    begin(static_cast<Phase>(phase_ + 1), now);
    if(done()) log_("BENCH done");
}

void Bench::step(uint32_t now)
{
    if(done()) return;
    if(phase_ < 0) {
        log_("BENCH start");
        begin(Warmup, now);
    }
    const uint32_t t = now - t0_;
    switch(phase_) {
        case Warmup:   /* a first start opens WiFi setup: close it so the home screen is measured */
            if(wifi_ && wifi_->setupOpen()) wifi_->cancelSetup(now);
            if(t >= kWarmupMs) finish(now);
            break;
        case Idle:
            if(t >= 5000) finish(now);
            break;
        case TurnUp:
        case TurnDown:
            if(acts_ < kSteps && t >= static_cast<uint32_t>(acts_) * kStepMs) {
                ui_.handle(InputEvent::rotate(phase_ == TurnUp ? 1 : -1));
                acts_++;
            }
            if(t >= kSteps * kStepMs) finish(now);
            break;
        case SettleUp:
        case SettleDown:   /* the adjusted target commits (BREW_COMMIT_MS) and the screen calms down */
            if(t >= 3500) finish(now);
            break;
        case Menu:   /* open, walk down to Previous, close: two view fades and four moves */
            if(acts_ == 0) {
                ui_.handle(InputEvent::press());
                acts_++;
            }
            else if(acts_ < 5 && t >= 1000 + static_cast<uint32_t>(acts_ - 1) * kStepMs) {
                ui_.handle(InputEvent::rotate(1));
                acts_++;
            }
            else if(acts_ == 5 && t >= 2500) {
                ui_.handle(InputEvent::press());
                acts_++;
            }
            if(t >= 3500) finish(now);
            break;
        case FullRedraw:   /* the worst case: every frame redraws the whole screen */
            lv_obj_invalidate(lv_screen_active());
            if(t >= 3000) finish(now);
            break;
        default:
            break;
    }
}

}  // namespace brew::app
