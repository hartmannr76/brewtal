/* KnobInput + TouchTracker. The press/turn ordering cases are the ones from the old
 * tools/models/press_turn_order.py timing model. */
#include "brew/input.h"
#include "minitest.h"

#include <string>
#include <vector>

using namespace brew;

namespace {

std::string fmt(const std::vector<InputEvent> & ev)
{
    std::string s;
    for(const InputEvent & e : ev) {
        if(!s.empty()) s += " ";
        if(e.kind == InputEvent::Kind::Press) s += "PRESS";
        else if(e.kind == InputEvent::Kind::Rotate) s += (e.a > 0 ? "+" : "") + std::to_string(e.a);
        else s += "T";
    }
    return s;
}

/* Physical script sampled every 5 ms: switch(t) and count(t). */
template<typename Sw, typename Cnt>
std::string simulate(Sw sw, Cnt cnt, uint32_t until, KnobInput::Config cfg = {})
{
    KnobInput k(nullptr, nullptr, cfg);
    std::vector<InputEvent> out;
    for(uint32_t t = 0; t < until; t += 5) k.step(t, cnt(t), sw(t), [&](const InputEvent & e) { out.push_back(e); });
    return fmt(out);
}

}  // namespace

TEST(knob_press_then_turn_keeps_order)
{
    /* press at 0, release at 120, one click clockwise `gap` ms after the release */
    for(uint32_t gap : { 5u, 15u, 40u, 200u }) {
        const uint32_t turn_t = 120 + gap;
        const std::string r = simulate([](uint32_t t) { return t < 120; },
                                       [&](uint32_t t) { return t >= turn_t ? 2 : 0; }, 600);
        CHECK_EQ(r, std::string("PRESS +1"));
    }
}

TEST(knob_turn_then_press_keeps_order)
{
    /* turn one click at 0, press at 50, release at 150 */
    const std::string r = simulate([](uint32_t t) { return t >= 50 && t < 150; },
                                   [](uint32_t t) { return t >= 0 ? 2 : 0; }, 600);
    CHECK_EQ(r, std::string("+1 PRESS"));
}

TEST(knob_detents_and_direction)
{
    CHECK_EQ(simulate([](uint32_t) { return false; }, [](uint32_t t) { return static_cast<int>(t / 50) * 2; }, 255),
             std::string("+1 +1 +1 +1 +1"));
    CHECK_EQ(simulate([](uint32_t) { return false; }, [](uint32_t t) { return t >= 20 ? -6 : 0; }, 100),
             std::string("-3"));
    KnobInput::Config inv;
    inv.invert = true;
    CHECK_EQ(simulate([](uint32_t) { return false; }, [](uint32_t t) { return t >= 20 ? 4 : 0; }, 100, inv),
             std::string("-2"));
}

TEST(knob_encoder_bounce_cancels_out)
{
    /* one channel chattering: count flips 0,1,0,1 ... never reaches a whole detent */
    CHECK_EQ(simulate([](uint32_t) { return false; }, [](uint32_t t) { return static_cast<int>((t / 5) % 2); }, 300),
             std::string(""));
}

TEST(knob_switch_bounce_is_one_press)
{
    /* press with 10 ms of contact chatter on both edges */
    auto sw = [](uint32_t t) {
        if(t < 10) return (t / 5) % 2 == 0;
        if(t < 100) return true;
        if(t < 110) return (t / 5) % 2 == 1;
        return false;
    };
    CHECK_EQ(simulate(sw, [](uint32_t) { return 0; }, 400), std::string("PRESS"));
}

TEST(knob_turning_while_held_cancels_press)
{
    /* hold the knob down and turn it 2 detents: rotation counts, the press does not */
    const std::string r = simulate([](uint32_t t) { return t < 300; },
                                   [](uint32_t t) { return t >= 100 ? 4 : 0; }, 600);
    CHECK_EQ(r, std::string("+2"));
}

TEST(touch_tracker_down_move_up_with_jitter)
{
    TouchTracker tt(4);
    std::vector<InputEvent> ev;
    auto emit = [&](const InputEvent & e) { ev.push_back(e); };
    hal::TouchPoint p;
    p.pressed = true; p.x = 100; p.y = 100;
    tt.step(p, emit);
    p.x = 102;                 // jitter: ignored
    tt.step(p, emit);
    p.x = 110;                 // real move
    tt.step(p, emit);
    p.pressed = false;
    tt.step(p, emit);
    tt.step(p, emit);          // still up: nothing
    CHECK_EQ(ev.size(), static_cast<size_t>(3));
    CHECK(ev[0].kind == InputEvent::Kind::TouchDown);
    CHECK(ev[1].kind == InputEvent::Kind::TouchMove);
    CHECK_EQ(ev[1].a, static_cast<int16_t>(110));
    CHECK(ev[2].kind == InputEvent::Kind::TouchUp);
    CHECK_EQ(ev[2].a, static_cast<int16_t>(110));
}

/* ---------------------------------------------------------------- ButtonInput */

namespace {

/* Runs ButtonInput every 5 ms for `ms`; each lambda gives a button's level at time t.
 * Returns the events like the knob tests: "+1 +1 PRESS". */
template<typename Up, typename Down, typename Sel>
std::string buttons(Up up, Down down, Sel sel, uint32_t ms, ButtonInput::Config cfg = {})
{
    ButtonInput b(nullptr, nullptr, nullptr, cfg);
    std::string s;
    for(uint32_t t = 0; t <= ms; t += 5) {
        b.step(t, up(t), down(t), sel(t), [&](const InputEvent & e) {
            if(!s.empty()) s += ' ';
            s += e.kind == InputEvent::Kind::Press ? std::string("PRESS") : (e.a > 0 ? "+" : "") + std::to_string(e.a);
        });
    }
    return s;
}
auto never = [](uint32_t) { return false; };

}  // namespace

TEST(buttons_tap_steps_once_and_select_presses_on_release)
{
    CHECK_EQ(buttons([](uint32_t t) { return t >= 10 && t < 100; }, never, never, 600), std::string("+1"));
    CHECK_EQ(buttons(never, [](uint32_t t) { return t >= 10 && t < 100; }, never, 600), std::string("-1"));
    CHECK_EQ(buttons(never, never, [](uint32_t t) { return t >= 10 && t < 100; }, 600), std::string("PRESS"));
    /* nothing until select is let go */
    CHECK_EQ(buttons(never, never, [](uint32_t t) { return t >= 10; }, 600), std::string(""));
}

TEST(buttons_chatter_is_debounced)
{
    /* 15 ms of contact bounce on press and release: still one step */
    auto up = [](uint32_t t) {
        if(t < 15) return (t / 5) % 2 == 0;
        if(t < 200) return true;
        if(t < 215) return (t / 5) % 2 == 1;
        return false;
    };
    CHECK_EQ(buttons(up, never, never, 400), std::string("+1"));
}

TEST(buttons_hold_repeats_after_a_delay)
{
    /* held 1 s: step at 25 ms (debounced), repeats from 475 ms every 120 ms -> 475, 595, 715, 835, 955 */
    const std::string r = buttons([](uint32_t t) { return t < 1000; }, never, never, 1500);
    CHECK_EQ(r, std::string("+1 +1 +1 +1 +1 +1"));
    /* released at 700 ms: the 715 ms repeat must not fire while the release debounces */
    CHECK_EQ(buttons([](uint32_t t) { return t < 700; }, never, never, 1200), std::string("+1 +1 +1"));
    ButtonInput::Config no_repeat;
    no_repeat.repeat_ms = 0;
    CHECK_EQ(buttons([](uint32_t t) { return t < 1000; }, never, never, 1500, no_repeat), std::string("+1"));
}

TEST(buttons_select_held_blocks_steps_and_cancels_repeat)
{
    /* up pressed while select is held: no step, and no burst after select is released */
    CHECK_EQ(buttons([](uint32_t t) { return t >= 100 && t < 900; }, never,
                     [](uint32_t t) { return t < 300; }, 1200),
             std::string("PRESS"));
    /* select pressed during a repeat stops it; nothing piles up for after.
     * Steps at 25, 475, 595 ms; select confirmed at 625 ms cancels the repeat. */
    CHECK_EQ(buttons([](uint32_t t) { return t < 1500; }, never,
                     [](uint32_t t) { return t >= 600 && t < 700; }, 2000),
             std::string("+1 +1 +1 PRESS"));
}
