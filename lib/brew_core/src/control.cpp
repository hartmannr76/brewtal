#include "brew/control.h"

#include <cstring>

namespace brew {

/* ------------------------------ OutputGuard ------------------------------ */

OutputGuard::OutputGuard(hal::DigitalOut * heat, hal::DigitalOut * cool, Config cfg)
    : heat_(heat), cool_(cool), cfg_(cfg)
{
}

void OutputGuard::drive()
{
    /* Interlock: switch the leaving output off before the arriving one on. */
    if(cur_ != Action::Heating && heat_) heat_->write(false);
    if(cur_ != Action::Cooling && cool_) cool_->write(false);
    if(cur_ == Action::Heating && heat_) heat_->write(true);
    if(cur_ == Action::Cooling && cool_) cool_->write(true);
}

uint32_t OutputGuard::blockedFor(Action a, uint32_t now) const
{
    if(a == Action::Idle || a == cur_) return 0;
    uint32_t wait = 0;
    auto need = [&](uint32_t since, uint32_t len) {
        const uint32_t el = now - since;
        if(el < len && len - el > wait) wait = len - el;
    };
    if(cur_ != Action::Idle) need(since_ms_, timing(cur_).min_on_ms);
    const int t = static_cast<int>(a);
    const int other = a == Action::Heating ? static_cast<int>(Action::Cooling) : static_cast<int>(Action::Heating);
    if(ever_[t]) need(off_ms_[t], timing(a).min_off_ms);   // restart delay since it last stopped
    if(ever_[other]) need(off_ms_[other], cfg_.changeover_ms);
    return wait;
}

Action OutputGuard::apply(Action want, uint32_t now)
{
    Action target = want;
    /* heat <-> cool with a changeover gap has to pass through idle first */
    if(cur_ != Action::Idle && want != Action::Idle && want != cur_ && cfg_.changeover_ms > 0)
        target = Action::Idle;

    if(target != cur_) {
        /* keep a running output on for its min_on */
        const bool can_stop = cur_ == Action::Idle || now - since_ms_ >= timing(cur_).min_on_ms;
        /* The wanted output can't start yet (restart delay, changeover): still switch off the
         * one that's no longer wanted, rather than keep heating a beer that wants cooling. */
        if(can_stop && target != Action::Idle && blockedFor(target, now) > 0) target = Action::Idle;
        if(can_stop && target != cur_) {
            if(cur_ != Action::Idle) {
                off_ms_[static_cast<int>(cur_)] = now;
                ever_[static_cast<int>(cur_)] = true;
            }
            cur_ = target;
            since_ms_ = now;
        }
    }
    drive();   // every call: relay drivers dedupe, and a failed write gets retried
    return cur_;
}

/* -------------------------------- History -------------------------------- */

void History::push(float c)
{
    if(n_ == kCapacity) {
        std::memmove(buf_.get(), buf_.get() + 1, (kCapacity - 1) * sizeof(float));
        n_--;
    }
    buf_[n_++] = c;
}

void History::assign(const float * c, int n)
{
    if(n > kCapacity) {
        c += n - kCapacity;
        n = kCapacity;
    }
    std::memcpy(buf_.get(), c, static_cast<size_t>(n) * sizeof(float));
    n_ = n;
}

}  // namespace brew
