/**
 * Input policy: turns raw encoder counts, a push-button level and touch samples into one
 * ordered stream of InputEvents. Hardware-free; drivers only supply raw values.
 */
#pragma once

#include "brew/drivers/interfaces/input.h"
#include "brew/drivers/interfaces/io.h"

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

/**
 * Three push buttons (up, down, select) -> the same Rotate/Press events as the knob, for
 * panels with keys instead of a rotary ring. Up = +1 (clockwise / warmer), down = -1.
 *
 *   - Each button is debounced on its own (debounce_ms).
 *   - Up/down step once when the press is confirmed, then repeat while held: first after
 *     repeat_delay_ms, then every repeat_ms (sweeping the target, like a thermostat).
 *   - Select emits Press when it is released, so a long-press can mean something else later
 *     without changing what a short press does.
 *   - While select is down, up/down are ignored (no accidental change while pressing).
 *
 * Any button may be null. Call poll() every ~5 ms from one task.
 */
class ButtonInput {
public:
    struct Config {
        uint16_t debounce_ms = 25;
        uint16_t repeat_delay_ms = 450;
        uint16_t repeat_ms = 120;       // 0 = no auto-repeat
        bool     active_low = true;
    };

    ButtonInput(hal::DigitalIn * up, hal::DigitalIn * down, hal::DigitalIn * select, const Config & cfg)
        : cfg_(cfg)
    {
        in_[kUp] = up;
        in_[kDown] = down;
        in_[kSelect] = select;
    }

    template<typename Emit>
    void poll(uint32_t now_ms, Emit && emit)
    {
        bool raw[3];
        for(int i = 0; i < 3; i++) raw[i] = in_[i] && (in_[i]->read() != cfg_.active_low);
        step(now_ms, raw[kUp], raw[kDown], raw[kSelect], emit);
    }

    /** The whole policy, on raw levels (true = pressed, polarity already corrected). */
    template<typename Emit>
    void step(uint32_t now, bool up, bool down, bool select, Emit && emit)
    {
        const bool raw[3] = { up, down, select };
        for(int i = 0; i < 3; i++) {
            Key & k = key_[i];
            if(raw[i] != k.raw) {
                k.raw = raw[i];
                k.changed = now;
            }
            if(k.raw == k.stable || now - k.changed < cfg_.debounce_ms) continue;
            k.stable = k.raw;
            if(i == kSelect) {
                if(k.stable) key_[kUp].repeating = key_[kDown].repeating = false;   // select wins
                else emit(InputEvent::press());
                continue;
            }
            k.repeating = k.stable && !key_[kSelect].stable;
            if(k.repeating) {
                emit(InputEvent::rotate(i == kUp ? 1 : -1));
                k.next = now + cfg_.repeat_delay_ms;
            }
        }
        if(cfg_.repeat_ms == 0 || key_[kSelect].stable) return;
        for(int i = kUp; i <= kDown; i++) {
            Key & k = key_[i];
            /* k.raw: stop the instant the contact opens; waiting for the release to debounce
             * would let one more repeat through after the finger has lifted */
            while(k.repeating && k.raw && static_cast<int32_t>(now - k.next) >= 0) {
                emit(InputEvent::rotate(i == kUp ? 1 : -1));
                k.next += cfg_.repeat_ms;
            }
        }
    }

private:
    enum { kUp, kDown, kSelect };
    struct Key {
        bool raw = false, stable = false;
        bool repeating = false;   // stepped on this press: auto-repeat is armed
        uint32_t changed = 0, next = 0;
    };

    hal::DigitalIn * in_[3] = {};
    Config cfg_;
    Key key_[3];
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
