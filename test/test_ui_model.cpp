/* The UI model (lib/ui/src/brew_model.c): what a turn or a press does in each view, the
 * auto-commit flags, history scrubbing, and the texts every layout shows. No LVGL involved. */
#include "brew_model.h"
#include "minitest.h"

#include <cstring>
#include <string>

namespace {

struct Calls {
    int setpoints = 0, modes = 0, units = 0, day_resets = 0, wifi_setups = 0, wifi_cancels = 0, wifi_forgets = 0, tunings = 0;
    int profiles = 0, steps = 0, counts = 0, last_count = 0, last_step = -1;
    brew_profile_t profile = BREW_PROFILE_CONSTANT;
    brew_step_t step = {};
    brew_tuning_t tuning = BREW_TUNE_N;
    float tuning_value = 0;
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
    cbs.on_wifi_setup = [](void * u) { static_cast<Calls *>(u)->wifi_setups++; };
    cbs.on_wifi_cancel = [](void * u) { static_cast<Calls *>(u)->wifi_cancels++; };
    cbs.on_wifi_forget = [](void * u) { static_cast<Calls *>(u)->wifi_forgets++; };
    cbs.on_profile = [](brew_profile_t p, void * u) {
        auto & k = *static_cast<Calls *>(u);
        k.profiles++;
        k.profile = p;
    };
    cbs.on_step = [](int i, float t, int r, int h, void * u) {
        auto & k = *static_cast<Calls *>(u);
        k.steps++;
        k.last_step = i;
        k.step = { t, r, h };
    };
    cbs.on_step_count = [](int n, void * u) {
        auto & k = *static_cast<Calls *>(u);
        k.counts++;
        k.last_count = n;
    };
    cbs.on_tuning = [](brew_tuning_t t, float v, void * u) {
        auto & k = *static_cast<Calls *>(u);
        k.tunings++;
        k.tuning = t;
        k.tuning_value = v;
    };
    return cbs;
}

std::string row(const brew_model_t & m, int i)
{
    brew_list_item_t it;
    brew_model_list_item(&m, i, &it);
    return it.value[0] ? std::string(it.label) + " = " + it.value : std::string(it.label);
}

/* Moves the selection to the row whose label starts with `label`; false if there is none. */
bool go(brew_model_t & m, const char * label)
{
    for(int i = 0; i < brew_model_list_count(&m); i++) {
        brew_list_item_t it;
        brew_model_list_item(&m, i, &it);
        if(std::strncmp(it.label, label, std::strlen(label)) == 0) {
            brew_model_encoder(&m, i - m.list_idx);
            return true;
        }
    }
    return false;
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
    CHECK(m.view == BREW_VIEW_LIST && m.list == BREW_LIST_MENU);
    CHECK(ch & BREW_CH_VIEW);
    CHECK_EQ(brew_model_list_count(&m), 5);
    CHECK(row(m, 0) == "Ferment" && row(m, 1) == "Cold crash" && row(m, 2) == "Pause" &&
          row(m, 3) == "Settings" && row(m, 4) == "Previous");
    CHECK_EQ(brew_model_encoder(&m, -1), 0u);    // already at the top: nothing moves
    CHECK_EQ(brew_model_encoder(&m, 1), static_cast<uint32_t>(BREW_CH_MENU_MOVE));
    brew_model_encoder(&m, 99);
    CHECK_EQ(m.list_idx, 4);                      // Previous
    brew_model_press(&m);
    CHECK(m.view == BREW_VIEW_HOME);

    /* Cold crash starts at its setting */
    brew_model_set_tuning(&m, 0.5f, 0.5f, 180, 180, 36.0f);
    brew_model_press(&m);
    go(m, "Cold crash");
    ch = brew_model_press(&m);
    CHECK(m.view == BREW_VIEW_HOME);
    CHECK(m.mode == BREW_MODE_CRASH);
    CHECK_NEAR(m.sp_f[1], 36.0, 0.01);
    CHECK(ch & BREW_CH_GRAPH);
    CHECK_EQ(c.modes, 1);
    CHECK(status(m) == "Crashing to 36\xC2\xB0");
    brew_model_encoder(&m, -2);                  // adjusted on the dial...
    brew_model_press(&m);
    CHECK_NEAR(m.sp_f[1], 35.0, 0.01);
    brew_model_press(&m);                        // ...and picking Cold crash again keeps it
    go(m, "Cold crash");
    CHECK(std::string(brew_model_press_hint(&m)) == "Close");
    brew_model_press(&m);
    CHECK_NEAR(m.sp_f[1], 35.0, 0.01);

    brew_model_press(&m);                        // menu; pause
    go(m, "Pause");
    brew_model_press(&m);
    CHECK(m.paused && c.paused);
    CHECK(status(m) == "Paused");
    CHECK(brew_model_vstate(&m) == BREW_VS_PAUSE);
    brew_model_press(&m);
    CHECK(row(m, 2) == "Resume");
    brew_list_item_t it;
    brew_model_list_item(&m, 1, &it);
    CHECK(it.mark == BREW_MARK_NONE);            // paused: no mode marked running
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
    CHECK(hint() == "Open");                     // Settings
    brew_model_encoder(&m, 1);
    CHECK(hint() == "Back");                     // Previous
    CHECK(!brew_model_can_step(&m, 1));

    brew_model_encoder(&m, -1);
    brew_model_press(&m);                        // Settings
    CHECK(hint() == "Switch");                   // Units
    go(m, "Heat");
    brew_model_press(&m);
    CHECK(hint() == "Edit");
    brew_model_press(&m);                        // the editor
    CHECK(m.view == BREW_VIEW_EDIT);
    CHECK(hint() == "Save");
    CHECK(brew_model_can_step(&m, 1) && brew_model_can_step(&m, -1));
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

static brew_wifi_t wifi(brew_wifi_phase_t p, brew_wifi_error_t e = BREW_WIFI_ERR_NONE, const char * ssid = "Hartmann-5G")
{
    brew_wifi_t w = {};
    w.phase = p;
    w.error = e;
    w.ssid = ssid;
    w.ap_ssid = "BrewDial-4F2A";
    w.ap_password = "malt-7391";
    return w;
}

TEST(ui_model_wifi_setup_opens_its_view_and_a_press_skips_it)
{
    Calls c;
    brew_model_t m;
    brew_model_init(&m, nullptr);
    m.cbs = cbs_for(c);
    brew_model_encoder(&m, 2);                   // mid-adjustment when setup starts
    brew_wifi_t w = wifi(BREW_WIFI_SETUP, BREW_WIFI_ERR_NONE, "");
    uint32_t ch = brew_model_set_wifi(&m, &w);
    CHECK(m.view == BREW_VIEW_WIFI);
    CHECK(!m.setting);
    CHECK(ch & BREW_CH_DISARM_COMMIT);
    CHECK(brew_model_wifi_screen(&m) == BREW_WIFI_SCREEN_JOIN);
    brew_wifi_text_t t;
    brew_model_wifi_text(&m, &t);
    CHECK(std::string(t.qr) == "WIFI:T:WPA;S:BrewDial-4F2A;P:malt-7391;;");
    CHECK(std::string(t.line1) == "BrewDial-4F2A \xC2\xB7 malt-7391");
    CHECK(std::string(brew_model_press_hint(&m)) == "Skip");
    CHECK_EQ(brew_model_encoder(&m, 3), 0u);     // turning does nothing here
    CHECK(brew_model_vstate(&m) == BREW_VS_COOL);

    brew_model_press(&m);
    CHECK(m.view == BREW_VIEW_HOME);
    CHECK_EQ(c.wifi_cancels, 1);
    w = wifi(BREW_WIFI_OFF, BREW_WIFI_ERR_NONE, "");
    brew_model_set_wifi(&m, &w);                 // WifiSetup confirms: setup closed
    CHECK(m.view == BREW_VIEW_HOME);
}

TEST(ui_model_wifi_follows_the_phone_to_connected_then_returns)
{
    Calls c;
    brew_model_t m;
    brew_model_init(&m, nullptr);
    m.cbs = cbs_for(c);
    brew_wifi_t w = wifi(BREW_WIFI_SETUP);
    brew_model_set_wifi(&m, &w);
    w = wifi(BREW_WIFI_PHONE);
    brew_model_set_wifi(&m, &w);
    CHECK(brew_model_wifi_screen(&m) == BREW_WIFI_SCREEN_PHONE);
    CHECK(std::string(brew_model_press_hint(&m)) == "Cancel");
    w = wifi(BREW_WIFI_CONNECTING);
    brew_model_set_wifi(&m, &w);
    CHECK(brew_model_wifi_screen(&m) == BREW_WIFI_SCREEN_TRYING);

    w = wifi(BREW_WIFI_PHONE, BREW_WIFI_ERR_PASSWORD);   // refused: back to the form, still open
    brew_model_set_wifi(&m, &w);
    CHECK(m.view == BREW_VIEW_WIFI);
    CHECK(brew_model_wifi_screen(&m) == BREW_WIFI_SCREEN_FAILED);
    CHECK(brew_model_vstate(&m) == BREW_VS_HEAT);
    brew_wifi_text_t t;
    brew_model_wifi_text(&m, &t);
    CHECK(std::string(t.title) == "Wrong password");
    CHECK(std::string(t.line1) == "for Hartmann-5G");

    w = wifi(BREW_WIFI_CONNECTING);
    brew_model_set_wifi(&m, &w);
    w = wifi(BREW_WIFI_CONNECTED);
    const uint32_t ch = brew_model_set_wifi(&m, &w);
    CHECK(ch & BREW_CH_ARM_WIFI_DONE);           // "Connected" shows, then closes by itself
    CHECK(m.view == BREW_VIEW_WIFI);
    CHECK(brew_model_wifi_screen(&m) == BREW_WIFI_SCREEN_DONE);
    CHECK(brew_model_vstate(&m) == BREW_VS_OK);
    CHECK(std::string(brew_model_press_hint(&m)) == "Done");
    brew_model_set_wifi(&m, &w);                 // the same status again changes nothing
    CHECK(m.view == BREW_VIEW_WIFI);
    brew_model_wifi_done(&m);
    CHECK(m.view == BREW_VIEW_HOME);
    CHECK_EQ(c.wifi_cancels, 0);
    CHECK_EQ(brew_model_wifi_done(&m), 0u);      // a late timer is harmless
}

TEST(ui_model_settings_wifi_list)
{
    Calls c;
    brew_model_t m;
    brew_model_init(&m, nullptr);
    m.cbs = cbs_for(c);
    brew_model_press(&m);
    go(m, "Settings");
    brew_model_press(&m);
    CHECK(m.list == BREW_LIST_SETTINGS);
    CHECK(row(m, 1) == "WiFi = none");
    go(m, "WiFi");
    brew_model_press(&m);
    CHECK(m.list == BREW_LIST_WIFI);
    char note[64];
    brew_model_list_note(&m, note, sizeof(note));
    CHECK(std::string(note) == "This display has no WiFi");
    CHECK_EQ(brew_model_list_count(&m), 1);      // just Previous
    brew_model_press(&m);
    CHECK(m.list == BREW_LIST_SETTINGS && m.list_idx == 1);   // back on the row that opened it

    brew_wifi_t w = wifi(BREW_WIFI_CONNECTED, BREW_WIFI_ERR_NONE, "A-very-long-network-name");
    brew_model_set_wifi(&m, &w);
    CHECK(row(m, 1) == "WiFi = A-very-long-ne...");
    brew_model_press(&m);
    brew_model_list_note(&m, note, sizeof(note));
    CHECK(std::string(note) == "On A-very-long-network-name");
    CHECK(row(m, 0) == "Change network" && row(m, 1) == "Forget network" && row(m, 2) == "Previous");
    CHECK(std::string(brew_model_press_hint(&m)) == "Set up");
    brew_model_press(&m);                        // Change network
    CHECK_EQ(c.wifi_setups, 1);
    CHECK(m.view == BREW_VIEW_HOME);             // until WifiSetup reports setup open:
    w = wifi(BREW_WIFI_SETUP, BREW_WIFI_ERR_NONE, "A-very-long-network-name");
    brew_model_set_wifi(&m, &w);
    CHECK(m.view == BREW_VIEW_WIFI);

    /* setup closed from the phone ("Set up later") while still connected: back to the dial */
    w = wifi(BREW_WIFI_CONNECTED, BREW_WIFI_ERR_NONE, "A-very-long-network-name");
    const uint32_t ch = brew_model_set_wifi(&m, &w);
    CHECK(m.view == BREW_VIEW_HOME);
    CHECK(!(ch & BREW_CH_ARM_WIFI_DONE));

    brew_model_press(&m);                        // Forget network
    go(m, "Settings");
    brew_model_press(&m);
    go(m, "WiFi");
    brew_model_press(&m);
    go(m, "Forget");
    brew_model_press(&m);
    CHECK_EQ(c.wifi_forgets, 1);
    w = wifi(BREW_WIFI_OFFLINE);
    brew_model_set_wifi(&m, &w);
    CHECK(m.view == BREW_VIEW_HOME);             // dropping offline never takes over the dial
}

TEST(ui_model_heat_and_cool_editors_save_through_the_callback)
{
    Calls c;
    brew_model_t m;
    brew_model_init(&m, nullptr);
    m.cbs = cbs_for(c);
    brew_model_set_tuning(&m, 0.5f, 0.5f, 180, 180, 34.0f);
    brew_model_press(&m);
    go(m, "Settings");
    brew_model_press(&m);
    go(m, "Heat");
    brew_model_press(&m);
    CHECK(row(m, 0) == "Differential = 0.5\xC2\xB0" "F");
    CHECK(row(m, 1) == "Restart delay = 3 min");
    brew_model_press(&m);                        // edit the differential
    brew_edit_text_t t;
    brew_model_edit_text(&m, &t);
    CHECK(std::string(t.title) == "HEAT \xC2\xB7 DIFFERENTIAL");
    CHECK(std::string(t.value) == "0.5");
    CHECK(std::string(t.explain) == "Heating starts 0.5\xC2\xB0" "F below the target");
    brew_model_encoder(&m, 3);                   // 0.8 F
    brew_model_press(&m);
    CHECK_EQ(c.tunings, 1);
    CHECK(c.tuning == BREW_TUNE_HEAT_DIFF);
    CHECK_NEAR(c.tuning_value, 0.8, 0.001);
    CHECK(m.view == BREW_VIEW_LIST && m.list == BREW_LIST_HEAT && m.list_idx == 0);
    CHECK(row(m, 0) == "Differential = 0.8\xC2\xB0" "F");

    go(m, "Restart");                            // restart delay, clamped at 0
    brew_model_press(&m);
    brew_model_encoder(&m, -10);
    brew_model_edit_text(&m, &t);
    CHECK(std::string(t.value) == "0" && std::string(t.unit) == "min");
    CHECK(std::string(t.explain) == "Heat may switch back on right away");
    brew_model_press(&m);
    CHECK(c.tuning == BREW_TUNE_HEAT_DELAY);
    CHECK_NEAR(c.tuning_value, 0.0, 0.001);

    go(m, "Previous");                           // Cool, in Celsius
    brew_model_press(&m);
    CHECK(m.list == BREW_LIST_SETTINGS && m.list_idx == 2);
    brew_model_set_units(&m, true);
    go(m, "Cool");
    brew_model_press(&m);
    CHECK(row(m, 2) == "Cold crash = 1.1\xC2\xB0" "C");
    go(m, "Cold crash");
    brew_model_press(&m);
    brew_model_encoder(&m, 2);                   // 1.0 -> 2.0 C (snapped to half degrees)
    brew_model_press(&m);
    CHECK(c.tuning == BREW_TUNE_CRASH);
    CHECK_NEAR(c.tuning_value, 35.6, 0.01);      // F
    CHECK_NEAR(m.crash_default_f, 35.6, 0.01);
}

TEST(ui_model_profile_list_and_brewfather_stub)
{
    Calls c;
    brew_model_t m;
    brew_model_init(&m, nullptr);
    m.cbs = cbs_for(c);
    brew_model_set_batch(&m, "Manual", 6);
    brew_model_press(&m);
    go(m, "Settings");
    brew_model_press(&m);
    CHECK(row(m, 4) == "Profile = Constant");
    go(m, "Profile");
    brew_model_press(&m);
    brew_list_item_t it;
    brew_model_list_item(&m, 0, &it);
    CHECK(std::string(it.label) == "Constant" && it.mark == BREW_MARK_CHOSEN);
    CHECK(row(m, 3) == "Restart day count = Day 6");
    go(m, "Restart");
    brew_model_press(&m);
    CHECK_EQ(c.day_resets, 1);
    CHECK(row(m, 3) == "Restart day count = Day 1");
    CHECK(m.view == BREW_VIEW_LIST);             // stays on the list
    go(m, "Brewfather");
    brew_model_press(&m);
    CHECK(m.list == BREW_LIST_BREWFATHER);
    CHECK(std::string(brew_model_list_title(&m)) == "BREWFATHER");
    char note[64];
    brew_model_list_note(&m, note, sizeof(note));
    CHECK(std::string(note) == "Coming soon. Needs WiFi.");
    brew_model_press(&m);                        // Previous
    CHECK(m.list == BREW_LIST_PROFILE && m.list_idx == 2);
}

TEST(ui_model_schedule_lists_editors_and_home_text)
{
    Calls c;
    brew_model_t m;
    brew_model_init(&m, nullptr);
    m.cbs = cbs_for(c);
    brew_model_set_mode(&m, BREW_MODE_CRASH, false);
    brew_model_press(&m);
    go(m, "Settings");
    brew_model_press(&m);
    go(m, "Profile");
    brew_model_press(&m);
    go(m, "Schedule");
    brew_model_press(&m);
    CHECK(m.list == BREW_LIST_SCHEDULE);
    char note[64];
    brew_model_list_note(&m, note, sizeof(note));
    CHECK_EQ(std::string(note), std::string("2 steps \xC2\xB7 8 d"));
    CHECK_EQ(row(m, 0), std::string("1 \xC2\xB7 66.0\xC2\xB0" "F = 4 d"));
    CHECK_EQ(row(m, 1), std::string("2 \xC2\xB7 70.0\xC2\xB0" "F = ramp 1 d, 3 d"));
    CHECK(row(m, 2) == "Add step" && row(m, 3) == "Use this schedule" && row(m, 4) == "Previous");

    /* edit step 2: 68 F, a 2-day ramp, a 5-day hold */
    go(m, "2 ");
    brew_model_press(&m);
    CHECK(m.list == BREW_LIST_STEP && m.step_sel == 1);
    CHECK(std::string(brew_model_list_title(&m)) == "STEP 2");
    CHECK(row(m, 1) == "Ramp = 1 d");
    brew_model_press(&m);                        // Temperature
    brew_edit_text_t t;
    brew_model_edit_text(&m, &t);
    CHECK(std::string(t.title) == "STEP 2 \xC2\xB7 TEMPERATURE");
    brew_model_encoder(&m, -4);
    brew_model_press(&m);
    CHECK_EQ(c.last_step, 1);
    CHECK_NEAR(c.step.temp_f, 68.0, 0.01);
    go(m, "Ramp");
    brew_model_press(&m);
    brew_model_encoder(&m, 1);
    brew_model_edit_text(&m, &t);
    CHECK(std::string(t.value) == "2" && std::string(t.unit) == "days");
    brew_model_press(&m);
    CHECK_EQ(c.step.ramp_h, 48);
    go(m, "Hold");
    brew_model_press(&m);
    brew_model_encoder(&m, 2);
    brew_model_press(&m);
    CHECK_EQ(c.step.hold_h, 120);
    CHECK(row(m, 2) == "Hold = 5 d");

    /* add a step (it copies the last), then remove step 1 */
    go(m, "Previous");
    brew_model_press(&m);
    CHECK(m.list == BREW_LIST_SCHEDULE && m.list_idx == 1);
    go(m, "Add step");
    brew_model_press(&m);
    CHECK_EQ(c.last_count, 3);
    CHECK(m.list == BREW_LIST_STEP && m.step_sel == 2);
    go(m, "Previous");
    brew_model_press(&m);
    go(m, "1 ");
    brew_model_press(&m);
    go(m, "Remove");
    brew_model_press(&m);
    CHECK_EQ(c.last_count, 2);
    CHECK_EQ(m.sched.nsteps, 2);
    brew_schedule_t one = m.sched;
    one.nsteps = 1;
    brew_model_set_schedule(&m, &one);
    brew_model_list_note(&m, note, sizeof(note));
    CHECK_EQ(std::string(note), std::string("1 step \xC2\xB7 7 d"));
    brew_model_set_schedule(&m, &m.sched);
    one.nsteps = 2;
    brew_model_set_schedule(&m, &one);
    CHECK_NEAR(m.sched.steps[0].temp_f, 68.0, 0.01);         // what was step 2 moved up

    /* use it: a schedule runs as Ferment */
    go(m, "Use");
    brew_model_press(&m);
    CHECK_EQ(c.profiles, 1);
    CHECK(c.profile == BREW_PROFILE_SCHEDULE);
    CHECK(m.view == BREW_VIEW_HOME);
    CHECK(m.mode == BREW_MODE_FERMENT);
    CHECK_EQ(c.modes, 1);

    char b[64];
    auto detail = [&] { brew_model_detail_text(&m, b, sizeof(b)); return std::string(b); };
    brew_schedule_t s = m.sched;
    s.step = 0;
    brew_model_set_schedule(&m, &s);
    CHECK(detail() == "Step 1/2 \xC2\xB7 ramp 2 d \xC2\xB7 hold 5 d");
    s.steps[1].ramp_h = 0;
    s.override_on = true;
    brew_model_set_schedule(&m, &s);
    CHECK(detail() == "Step 1/2 \xC2\xB7 your target until step 2");
    s.override_on = false;
    s.step = 1;
    brew_model_set_schedule(&m, &s);
    CHECK(detail() == "Step 2/2 \xC2\xB7 hold 5 d");                    // a jump: no ramp shown
    s.step = BREW_STEP_DONE;
    brew_model_set_schedule(&m, &s);
    CHECK(detail() == "Schedule done");
    s.override_on = true;
    brew_model_set_schedule(&m, &s);
    CHECK(detail() == "Schedule done \xC2\xB7 your target");
    s.override_on = false;
    brew_model_set_schedule(&m, &s);

    /* the Profile list shows it chosen, and Constant goes back */
    brew_model_press(&m);
    go(m, "Settings");
    brew_model_press(&m);
    CHECK(row(m, 4) == "Profile = Schedule");
    go(m, "Profile");
    brew_model_press(&m);
    brew_list_item_t it;
    brew_model_list_item(&m, 1, &it);
    CHECK(it.mark == BREW_MARK_CHOSEN);
    brew_model_press(&m);                        // Constant (row 0)
    CHECK(c.profile == BREW_PROFILE_CONSTANT);
    CHECK(m.sched.profile == BREW_PROFILE_CONSTANT);
}

