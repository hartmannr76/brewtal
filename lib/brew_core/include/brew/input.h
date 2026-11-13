/**
 * Input policy: turns raw encoder counts, a push-button level and touch samples into one
 * ordered stream of InputEvents. Hardware-free; drivers only supply raw values.
 */
#pragma once

#include "brew/hal/input.h"
#include "brew/hal/io.h"

#include <cstdint>
#include <cstdlib>

namespace brew {

struct InputEvent {
    enum class Kind : uint8_t { Rotate, Press, TouchDown, TouchMove, TouchUp };
    Kind    kind = Kind::Rotate;
    int16_t a = 0;   // Rotate: detents (+ = clockwise). Touch: x
    int16_t b = 0;   // Touch: y

    static InputEvent rotate(int d) { return { Kind::Rotate, static_cast<int16_t>(d), 0 }; }
    static InputEvent press() { return { Kind::Press, 0, 0 }; }
};

/**
 * Rotary encoder + push switch -> Rotate/Press events in true physical order.
 *
 * A press is stamped with the encoder count at the switch's first edge (not after debounce),
 * and while a release is still debouncing, rotation past that edge is held back. So "click,
 * then immediately turn" is never replayed as "turn, then click". Turning while holding the
 * button down by >= press_cancel_detents cancels the press. Bounce on an encoder channel
 * moves the count +/-1 and cancels out because only whole detents are emitted.
 *
 * Call poll() every ~5 ms from one task.
 */
class KnobInput {
public:
    struct Config {
        int      counts_per_detent = 2;     // EC28A: 30 detents, 15 pulses/rev -> 2
        bool     invert = false;            // flip if clockwise lowers the target
        uint16_t debounce_ms = 25;
        int      press_cancel_detents = 2;
        uint16_t hold_timeout_ms = 200;     // stop holding rotation if the switch keeps chattering
        bool     button_active_low = true;
    };

    KnobInput(hal::Encoder * enc, hal::DigitalIn * button, const Config & cfg)
        : enc_(enc), btn_(button), cfg_(cfg)
    {
    }

    /** Start counting from the encoder's current position. */
    void begin()
    {
        applied_ = enc_ ? enc_->count() : 0;
    }

    template<typename Emit>
    void poll(uint32_t now_ms, Emit && emit)
    {
        const int32_t count = enc_ ? enc_->count() : 0;
        bool down = false;
        if(btn_) down = btn_->read() != cfg_.button_active_low;
        step(now_ms, count, down, emit);
    }

    /** The whole policy, on raw values. `down` = switch pressed (already polarity-corrected). */
    template<typename Emit>
    void step(uint32_t now, int32_t count, bool down, Emit && emit)
    {
        if(down != raw_) {
            if(raw_ == stable_) edge_ = count;   // first edge of a transition
            raw_ = down;
            changed_ = now;
            if(stable_ && !raw_) {               // release starting: hold rotation at the edge
                hold_ = true;
                hold_count_ = edge_;
                hold_ms_ = now;
            }
            else if(raw_ == stable_) {           // bounced back: not a transition
                hold_ = false;
            }
        }
        if(raw_ != stable_ && now - changed_ >= cfg_.debounce_ms) {
            stable_ = raw_;
            if(stable_) {
                down_count_ = edge_;
            }
            else {
                if(std::abs(edge_ - down_count_) < cfg_.press_cancel_detents * cfg_.counts_per_detent) {
                    if(hold_) rotateTo(edge_, emit);   // rotation that happened before the release
                    emit(InputEvent::press());
                }
                hold_ = false;
            }
        }
        if(hold_ && now - hold_ms_ > cfg_.hold_timeout_ms) hold_ = false;
        rotateTo(hold_ ? hold_count_ : count, emit);
    }

private:
    template<typename Emit>
    void rotateTo(int32_t target, Emit && emit)
    {
        const int32_t det = (target - applied_) / cfg_.counts_per_detent;   // truncates toward 0
        if(det == 0) return;
        applied_ += det * cfg_.counts_per_detent;
        emit(InputEvent::rotate(cfg_.invert ? -det : det));
    }

    hal::Encoder * enc_;
    hal::DigitalIn * btn_;
    Config cfg_;

    int32_t applied_ = 0;
    bool raw_ = false, stable_ = false;
    uint32_t changed_ = 0;
    int32_t edge_ = 0, down_count_ = 0;
    bool hold_ = false;
    int32_t hold_count_ = 0;
    uint32_t hold_ms_ = 0;
};

/** Touch samples -> TouchDown/Move/Up, ignoring sub-`jitter` movement. */
class TouchTracker {
public:
    explicit TouchTracker(int16_t jitter_px = 4) : jitter_(jitter_px) {}

    template<typename Emit>
    void step(const hal::TouchPoint & p, Emit && emit)
    {
        if(p.pressed) {
            if(!down_ || std::abs(p.x - x_) >= jitter_ || std::abs(p.y - y_) >= jitter_) {
                x_ = p.x;
                y_ = p.y;
                emit(InputEvent{ down_ ? InputEvent::Kind::TouchMove : InputEvent::Kind::TouchDown, x_, y_ });
            }
            down_ = true;
        }
        else if(down_) {
            down_ = false;
            emit(InputEvent{ InputEvent::Kind::TouchUp, x_, y_ });
        }
    }

private:
    int16_t jitter_;
    bool down_ = false;
    int16_t x_ = 0, y_ = 0;
};

}  // namespace brew
