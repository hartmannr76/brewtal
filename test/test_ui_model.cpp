/* The UI model (lib/ui/src/brew_model.c): what a turn or a press does in each view, the
 * auto-commit flags, history scrubbing, and the texts every layout shows. No LVGL involved. */
#include "brew_model.h"
#include "minitest.h"

#include <cstring>
#include <string>

namespace {

struct Calls {
    int setpoints = 0, modes = 0, units = 0, day_resets = 0;
    brew_mode_t mode = BREW_MODE_FERMENT;
    float temp_f = 0;
    bool paused = false, celsius = false;
};

brew_ui_cbs_t cbs_for(Calls & c)
{
    brew_ui_cbs_t cbs = {};
    cbs.user = &c;
    cbs.on_setpoint = [](brew_mode_t m, float f, void * u) {
        auto & k = *static_cast<Calls *>(u);
        k.setpoints++;
        k.mode = m;
        k.temp_f = f;
    };
    cbs.on_mode = [](brew_mode_t m, bool p, void * u) {
        auto & k = *static_cast<Calls *>(u);
        k.modes++;
        k.mode = m;
        k.paused = p;
    };
    cbs.on_units = [](bool c, void * u) {
        auto & k = *static_cast<Calls *>(u);
        k.units++;
        k.celsius = c;
    };
    cbs.on_reset_day = [](void * u) { static_cast<Calls *>(u)->day_resets++; };
    return cbs;
}

std::string status(const brew_model_t & m)
{
    char b[64];
    brew_model_status_text(&m, b, sizeof(b));
    return b;
}

}  // namespace

TEST(ui_model_turning_adjusts_a_draft_and_arms_the_auto_commit)
{
    Calls c;
    brew_model_t m;
    brew_model_init(&m, nullptr);
    m.cbs = cbs_for(c);

    uint32_t ch = brew_model_encoder(&m, 3);
    CHECK(m.setting);
    CHECK_NEAR(m.draft_f, 67.5, 1e-6);           // 66 + 3 half-degree detents
    CHECK_NEAR(m.sp_f[0], 66.0, 1e-6);           // the real target is untouched until commit
    CHECK(ch & BREW_CH_ARM_COMMIT);
    CHECK(ch & BREW_CH_REFRESH);
    CHECK_NEAR(brew_model_shown_target(&m), 67.5, 1e-6);
    CHECK_EQ(c.setpoints, 0);

    ch = brew_model_commit(&m);                  // the countdown expired
    CHECK(!m.setting);
    CHECK(ch & BREW_CH_DISARM_COMMIT);
    CHECK_NEAR(m.sp_f[0], 67.5, 1e-6);
    CHECK_EQ(c.setpoints, 1);
    CHECK_NEAR(c.temp_f, 67.5, 1e-6);
    CHECK_EQ(brew_model_commit(&m), 0u);         // nothing left to commit
    CHECK_EQ(c.setpoints, 1);
}

TEST(ui_model_press_commits_and_the_range_is_clamped)
{
    Calls c;
    brew_model_t m;
    brew_model_init(&m, nullptr);
    m.cbs = cbs_for(c);
    brew_model_encoder(&m, 200);
    CHECK_NEAR(m.draft_f, BREW_T_MAX, 1e-6);
    brew_model_encoder(&m, -500);
    CHECK_NEAR(m.draft_f, BREW_T_MIN, 1e-6);
    const uint32_t ch = brew_model_press(&m);    // press = commit now
    CHECK(ch & BREW_CH_DISARM_COMMIT);
    CHECK_EQ(c.setpoints, 1);
    CHECK_NEAR(m.sp_f[0], BREW_T_MIN, 1e-6);
    CHECK(m.view == BREW_VIEW_HOME);
    CHECK_EQ(brew_model_encoder(&m, 0), 0u);
}

TEST(ui_model_celsius_steps_are_half_a_degree_c)
{
    brew_model_t m;
    brew_model_init(&m, nullptr);
    brew_model_set_units(&m, true);
    brew_model_set_setpoints(&m, 68.0f, 34.0f);   // 20.0 C
    brew_model_encoder(&m, 1);
    CHECK_NEAR((m.draft_f - 32.0) * 5.0 / 9.0, 20.5, 1e-4);
    char t[16];
    brew_model_fmt_temp(&m, t, sizeof(t), m.draft_f);
    CHECK(std::strcmp(t, "20.5") == 0);
}

TEST(ui_model_menu_navigation_and_choices)
{
    Calls c;
    brew_model_t m;
    brew_model_init(&m, nullptr);
    m.cbs = cbs_for(c);

    uint32_t ch = brew_model_press(&m);          // home -> menu
    CHECK(m.view == BREW_VIEW_MENU);
    CHECK(ch & BREW_CH_VIEW);
    CHECK_EQ(m.menu_idx, 0);
    CHECK_EQ(brew_model_encoder(&m, -1), 0u);    // already at the top: nothing moves
    CHECK_EQ(brew_model_encoder(&m, 1), static_cast<uint32_t>(BREW_CH_MENU_MOVE));
    CHECK_EQ(m.menu_idx, 1);
    brew_model_encoder(&m, 99);
    CHECK_EQ(m.menu_idx, BREW_MENU_N - 1);

    brew_model_encoder(&m, -(BREW_MENU_N - 1 - BREW_MENU_CRASH));
    CHECK_EQ(m.menu_idx, static_cast<int>(BREW_MENU_CRASH));
    ch = brew_model_press(&m);                   // pick Cold crash
    CHECK(m.view == BREW_VIEW_HOME);
    CHECK(m.mode == BREW_MODE_CRASH);
    CHECK(ch & BREW_CH_GRAPH);
    CHECK_EQ(c.modes, 1);
    CHECK(c.mode == BREW_MODE_CRASH && !c.paused);
    CHECK(brew_model_menu_active(&m, BREW_MENU_CRASH));
    CHECK(!brew_model_menu_active(&m, BREW_MENU_FERMENT));
    CHECK(status(m) == "Crashing to 34\xC2\xB0");

    brew_model_press(&m);                        // menu again; pause
    brew_model_encoder(&m, BREW_MENU_PAUSE);
    brew_model_press(&m);
    CHECK(m.paused);
    CHECK(c.paused);
    CHECK(status(m) == "Paused");
    CHECK(brew_model_vstate(&m) == BREW_VS_PAUSE);
    char label[32];
    brew_model_menu_label(&m, BREW_MENU_PAUSE, label, sizeof(label));
    CHECK(std::strcmp(label, "Resume") == 0);
    CHECK(!brew_model_menu_active(&m, BREW_MENU_CRASH));   // paused: no mode marked

    brew_model_press(&m);                        // menu; units
    brew_model_encoder(&m, BREW_MENU_UNITS);
    ch = brew_model_press(&m);
    CHECK(m.celsius && c.celsius);
    CHECK_EQ(c.units, 1);
    CHECK(m.view == BREW_VIEW_MENU);             // units toggles in place
    CHECK(ch & BREW_CH_REFRESH);
    brew_model_menu_label(&m, BREW_MENU_UNITS, label, sizeof(label));
    CHECK(std::strcmp(label, "Units \xC2\xB7 \xC2\xB0" "C") == 0);

    /* reset day count: last item; Day 1 shows straight away and the controller is told */
    brew_model_set_batch(&m, "Manual", 6);
    brew_model_encoder(&m, 99);
    CHECK_EQ(m.menu_idx, static_cast<int>(BREW_MENU_RESET_DAY));
    brew_model_menu_label(&m, BREW_MENU_RESET_DAY, label, sizeof(label));
    CHECK(std::strcmp(label, "Reset day count") == 0);
    ch = brew_model_press(&m);
    CHECK_EQ(c.day_resets, 1);
    CHECK_EQ(m.day, 1);
    CHECK(m.view == BREW_VIEW_HOME);
    CHECK(ch & BREW_CH_REFRESH);
}

TEST(ui_model_status_follows_the_action)
{
    brew_model_t m;
    brew_model_init(&m, nullptr);
    CHECK(status(m) == "Holding at 66\xC2\xB0");
    CHECK(brew_model_vstate(&m) == BREW_VS_IDLE);
    brew_model_set_action(&m, BREW_ACTION_COOLING);
    CHECK(status(m) == "Cooling to 66\xC2\xB0");
    brew_model_set_action(&m, BREW_ACTION_HEATING);
    CHECK(status(m) == "Heating to 66\xC2\xB0");
    CHECK(brew_model_vstate(&m) == BREW_VS_HEAT);
    brew_model_set_batch(&m, "Pale ale", 4);
    char b[64];
    brew_model_batch_text(&m, b, sizeof(b));
    CHECK(std::strcmp(b, "Pale ale \xC2\xB7 Day 4") == 0);
}

TEST(ui_model_status_says_when_a_held_off_output_will_start)
{
    brew_model_t m;
    brew_model_init(&m, nullptr);
    brew_model_set_temp(&m, 68.6f);
    /* the controller wants to cool, the restart delay holds it off */
    CHECK(brew_model_set_pending(&m, BREW_ACTION_COOLING, 300) & BREW_CH_REFRESH);
    CHECK(brew_model_waiting(&m));
    CHECK(status(m) == "Cooling in 5 min");
    CHECK(brew_model_vstate(&m) == BREW_VS_IDLE);   // the relays are off: idle colours
    CHECK_EQ(brew_model_set_pending(&m, BREW_ACTION_COOLING, 299), 0u);   // same minute: no redraw
    brew_model_set_pending(&m, BREW_ACTION_COOLING, 59);
    CHECK(status(m) == "Cooling in 1 min");
    brew_model_set_pending(&m, BREW_ACTION_COOLING, 0);
    CHECK(status(m) == "Cooling in 1 min");          // about to start: never "in 0 min"
    brew_model_set_action(&m, BREW_ACTION_COOLING);
    CHECK(!brew_model_waiting(&m));
    CHECK(status(m) == "Cooling to 66\xC2\xB0");
    /* changeover gap before heating */
    brew_model_set_action(&m, BREW_ACTION_IDLE);
    brew_model_set_pending(&m, BREW_ACTION_HEATING, 540);
    CHECK(status(m) == "Heating in 9 min");
    /* cold crash waiting on cooling's restart delay */
    brew_model_set_mode(&m, BREW_MODE_CRASH, false);
    brew_model_set_pending(&m, BREW_ACTION_COOLING, 200);
    CHECK(status(m) == "Crashing in 4 min");
    /* paused: nothing is waiting */
    brew_model_set_mode(&m, BREW_MODE_CRASH, true);
    CHECK(!brew_model_waiting(&m));
    CHECK(status(m) == "Paused");
    /* nothing wanted: back to holding */
    brew_model_set_mode(&m, BREW_MODE_FERMENT, false);
    brew_model_set_pending(&m, BREW_ACTION_IDLE, 0);
    CHECK(status(m) == "Holding at 66\xC2\xB0");
}

TEST(ui_model_first_reading_redraws_the_graph_later_ones_may_throttle)
{
    brew_model_t m;
    brew_model_init(&m, nullptr);
    CHECK_EQ(brew_model_hist_last(&m), -1);      // nothing to draw yet
    uint32_t ch = brew_model_set_temp(&m, 68.4f);
    CHECK(ch & BREW_CH_GRAPH);
    CHECK(ch & BREW_CH_TEMP);
    ch = brew_model_set_temp(&m, 68.5f);
    CHECK(!(ch & BREW_CH_GRAPH));
    CHECK(ch & BREW_CH_TEMP);
    CHECK_EQ(brew_model_hist_last(&m), 0);       // just the live point
    CHECK_EQ(brew_model_push_history(&m, 68.0f), static_cast<uint32_t>(BREW_CH_GRAPH_LAZY));
}

TEST(ui_model_history_scrub_and_rolling_window)
{
    brew_model_t m;
    brew_model_init(&m, nullptr);
    const float h[3] = { 60.0f, 61.0f, 62.0f };
    brew_model_set_history(&m, h, 3, 60000);     // minute samples: one sample per scrub step
    brew_model_set_temp(&m, 63.0f);
    CHECK_EQ(brew_model_hist_last(&m), 3);       // 3 samples + live
    CHECK_NEAR(brew_model_hist_val(&m, 3), 63.0, 1e-6);

    brew_model_press(&m);                        // menu -> History
    brew_model_encoder(&m, BREW_MENU_HISTORY);
    brew_model_press(&m);
    CHECK(m.view == BREW_VIEW_HIST);
    CHECK_EQ(m.hist_idx, 3);                     // starts at "now"
    char t[32];
    brew_model_hist_time(&m, m.hist_idx, t, sizeof(t));
    CHECK(std::strcmp(t, "Now") == 0);
    CHECK_EQ(brew_model_encoder(&m, 5), 0u);     // can't scrub past now
    brew_model_encoder(&m, -2);
    CHECK_EQ(m.hist_idx, 1);
    CHECK_NEAR(brew_model_hist_val(&m, m.hist_idx), 61.0, 1e-6);
    brew_model_hist_time(&m, m.hist_idx, t, sizeof(t));
    CHECK(std::strcmp(t, "Now") != 0);
    brew_model_encoder(&m, -9);
    CHECK_EQ(m.hist_idx, 0);

    /* fill the window: the scrub position stays on the same sample as old ones drop off */
    m.hist_idx = 2;                              // the 62 F sample
    for(int i = 3; i < BREW_HIST_MAX; i++) brew_model_push_history(&m, 70.0f);
    CHECK_EQ(m.hist_n, BREW_HIST_MAX);
    CHECK_EQ(m.hist_idx, 2);
    brew_model_push_history(&m, 71.0f);          // oldest drops off
    CHECK_EQ(m.hist_n, BREW_HIST_MAX);
    CHECK_EQ(m.hist_idx, 1);
    CHECK_NEAR(brew_model_hist_val(&m, m.hist_idx), 62.0, 1e-6);

    brew_model_press(&m);                        // back home
    CHECK(m.view == BREW_VIEW_HOME);
}

TEST(ui_model_history_window_text_follows_the_sample_interval)
{
    brew_model_t m;
    brew_model_init(&m, nullptr);
    char t[32];
    auto window = [&](int ms_per) {
        const float one = 60.0f;
        brew_model_set_history(&m, &one, 1, ms_per);
        brew_model_hist_window_text(&m, t, sizeof(t));
        return std::string(t);
    };
    CHECK(window(200) == "last hour");     // 18000 samples x 200 ms (the default)
    CHECK(window(100) == "last 30 min");
    CHECK(window(400) == "last 2 hours");
    CHECK(window(300) == "last 1h 30m");
}

TEST(ui_model_two_second_history_scrubs_a_minute_per_step)
{
    brew_model_t m;
    brew_model_init(&m, nullptr);
    static float h[300];                          // 10 min at 2 s
    for(int i = 0; i < 300; i++) h[i] = 60.0f + 0.01f * (float)i;
    brew_model_set_history(&m, h, 300, 2000);
    brew_model_set_temp(&m, 64.0f);
    brew_model_press(&m);                         // menu -> History
    brew_model_encoder(&m, BREW_MENU_HISTORY);
    brew_model_press(&m);
    CHECK_EQ(m.hist_idx, 300);                    // "now"
    CHECK_EQ(brew_model_hist_step(&m), 30);       // 60 s / 2 s
    brew_model_encoder(&m, -1);
    CHECK_EQ(m.hist_idx, 270);
    char t[32];
    brew_model_hist_time(&m, m.hist_idx, t, sizeof(t));
    CHECK(std::strcmp(t, "0h 01m ago") == 0 || std::strchr(t, ':') != nullptr);   // or a wall-clock time
    brew_model_hist_time(&m, 290, t, sizeof(t));
    CHECK(std::strcmp(t, "20 s ago") == 0 || std::strchr(t, ':') != nullptr);
    brew_model_encoder(&m, -20);                  // clamps at the oldest sample
    CHECK_EQ(m.hist_idx, 0);
    CHECK(!brew_model_can_step(&m, -1));
    CHECK(brew_model_can_step(&m, 1));
    brew_model_encoder(&m, 50);                   // and at "now"
    CHECK_EQ(m.hist_idx, 300);
    CHECK(!brew_model_can_step(&m, 1));
}

TEST(ui_model_key_hints_say_what_the_keys_do)
{
    brew_model_t m;
    brew_model_init(&m, nullptr);
    brew_model_set_setpoints(&m, 66.0f, 34.0f);
    auto hint = [&] { return std::string(brew_model_press_hint(&m)); };

    CHECK(hint() == "Menu");
    CHECK(brew_model_can_step(&m, 1) && brew_model_can_step(&m, -1));
    brew_model_encoder(&m, 1);
    CHECK(hint() == "Set");
    brew_model_encoder(&m, 999);                 // clamped at the top of the range
    CHECK(!brew_model_can_step(&m, 1) && brew_model_can_step(&m, -1));
    brew_model_press(&m);                        // commit

    brew_model_press(&m);                        // menu, on Ferment (the running mode)
    CHECK(hint() == "Close");
    CHECK(!brew_model_can_step(&m, -1) && brew_model_can_step(&m, 1));
    brew_model_encoder(&m, 1);
    CHECK(hint() == "Start");                    // Cold crash
    brew_model_encoder(&m, 1);
    CHECK(hint() == "Pause");
    brew_model_encoder(&m, 1);
    CHECK(hint() == "Open");
    brew_model_encoder(&m, 1);
    CHECK(hint() == "Switch");
    brew_model_encoder(&m, 1);
    CHECK(hint() == "Reset");                    // Reset day count, the last item
    CHECK(!brew_model_can_step(&m, 1));

    brew_model_encoder(&m, -2);
    brew_model_press(&m);                        // History
    CHECK(m.view == BREW_VIEW_HIST);
    CHECK(hint() == "Back");
    CHECK(!brew_model_can_step(&m, 1) && !brew_model_can_step(&m, -1));   // nothing recorded yet
    const float h[3] = { 66.0f, 66.5f, 67.0f };
    brew_model_set_history(&m, h, 3, 1000);
    CHECK(!brew_model_can_step(&m, 1) && brew_model_can_step(&m, -1));   // at "now"

    brew_model_press(&m);                        // back home, then pause from the menu
    brew_model_press(&m);
    brew_model_encoder(&m, BREW_MENU_PAUSE);
    brew_model_press(&m);
    CHECK(m.paused);
    brew_model_press(&m);
    CHECK(hint() == "Start");                    // paused: no mode is running
    brew_model_encoder(&m, BREW_MENU_PAUSE);
    CHECK(hint() == "Resume");
}

TEST(ui_model_graph_style_defaults_to_hairline_and_redraws_only_on_change)
{
    brew_model_t m;
    brew_model_init(&m, nullptr);
    CHECK(m.graph_style == BREW_GRAPH_HAIRLINE);
    CHECK_EQ(brew_model_set_graph_style(&m, BREW_GRAPH_HAIRLINE), 0u);
    CHECK(brew_model_set_graph_style(&m, BREW_GRAPH_MIST) & BREW_CH_GRAPH);
    CHECK(m.graph_style == BREW_GRAPH_MIST);
    brew_model_set_graph_style(&m, static_cast<brew_graph_style_t>(7));   // unknown: hairline
    CHECK(m.graph_style == BREW_GRAPH_HAIRLINE);
}

TEST(ui_model_200ms_history_scrubs_a_minute_per_step)
{
    brew_model_t m;
    brew_model_init(&m, nullptr);
    static float h[1200];                         // 4 min at 200 ms (the default interval)
    for(int i = 0; i < 1200; i++) h[i] = 60.0f;
    brew_model_set_history(&m, h, 1200, 0);       // 0: keep the default
    CHECK_EQ(m.hist_ms_per, 200);
    CHECK_EQ(brew_model_hist_step(&m), 300);      // 60 s / 200 ms
    char t[32];
    brew_model_hist_time(&m, 1200 - 50, t, sizeof(t));   // 50 samples back = 10 s
    CHECK(std::strcmp(t, "10 s ago") == 0 || std::strchr(t, ':') != nullptr);
}

TEST(ui_model_pill_and_detail_texts)
{
    brew_model_t m;
    brew_model_init(&m, nullptr);
    char b[64];
    auto pill = [&]() { brew_model_pill_text(&m, b, sizeof(b)); return std::string(b); };
    auto detail = [&]() { brew_model_detail_text(&m, b, sizeof(b)); return std::string(b); };
    brew_model_set_batch(&m, "Pale ale", 4);
    brew_model_set_temp(&m, 68.0f);
    brew_model_set_action(&m, BREW_ACTION_COOLING);
    CHECK(pill() == "COOLING");
    CHECK(detail() == "Target 66.0\xC2\xB0 \xC2\xB7 Pale ale \xC2\xB7 Day 4");
    CHECK(std::string(brew_model_unit(&m)) == "\xC2\xB0" "F");
    brew_model_set_action(&m, BREW_ACTION_IDLE);
    CHECK(pill() == "HOLDING");
    brew_model_set_pending(&m, BREW_ACTION_COOLING, 150);
    CHECK(pill() == "COOLING IN 3 MIN");
    brew_model_set_pending(&m, BREW_ACTION_IDLE, 0);
    brew_model_set_action(&m, BREW_ACTION_HEATING);
    CHECK(pill() == "HEATING");
    brew_model_set_mode(&m, BREW_MODE_CRASH, false);
    CHECK(pill() == "CRASHING");
    CHECK(detail() == "Target 34.0\xC2\xB0 \xC2\xB7 Pale ale \xC2\xB7 Cold crash");
    brew_model_set_mode(&m, BREW_MODE_CRASH, true);
    CHECK(pill() == "PAUSED");
    brew_model_set_mode(&m, BREW_MODE_FERMENT, false);
    brew_model_encoder(&m, 1);                       // start adjusting the target
    CHECK(m.setting);
    CHECK(pill() == "SET TARGET");
    CHECK(detail() == "Turn to adjust \xC2\xB7 press to set");
    brew_model_set_units(&m, true);
    CHECK(std::string(brew_model_unit(&m)) == "\xC2\xB0" "C");
}
