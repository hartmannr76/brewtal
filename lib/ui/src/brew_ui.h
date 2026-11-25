/**
 * brew_ui.h - fermentation controller UI (LVGL 9): round dial or square panel.
 *
 * The UI is a pure view: it never runs the control loop. Your controller
 * feeds it readings/state through the setters below, and gets user intent
 * back through the callbacks. Input comes from a rotary encoder (detents)
 * and a press (encoder push or screen tap).
 *
 * All temperatures crossing this API are degrees Fahrenheit; the UI handles
 * display units itself.
 */
#pragma once

/* Per-target settings (BREW_UI_DISP_SIZE, BREW_UI_ALLOC, BREW_FADE_MS, BREW_BG_FADE_MS) normally
 * come from build flags in platformio.ini / sim/CMakeLists.txt. A brew_config.h on the include
 * path is still honoured. */
#if defined(__has_include)
#if __has_include("brew_config.h")
#include "brew_config.h"
#endif
#endif

#include "lvgl.h"
#include "brew_ui_types.h"   /* brew_mode_t, brew_action_t, brew_ui_cbs_t (no LVGL) */
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BREW_UI_DISP_SIZE
#define BREW_UI_DISP_SIZE 360   /* square, round panel; fonts are generated for 360 */
#endif

struct brew_layout;

/** Build the UI on `screen` (usually lv_screen_active()) with the round dial layout. */
void brew_ui_create(lv_obj_t * screen, const brew_ui_cbs_t * cbs);
/** Same, with a chosen layout (see brew_layout.h); NULL = round. */
void brew_ui_create_with_layout(lv_obj_t * screen, const brew_ui_cbs_t * cbs, const struct brew_layout * layout);
/** Swap the layout at run time, keeping the model (targets, view, an adjustment in progress). */
void brew_ui_set_layout(const struct brew_layout * layout);
/** "round", "square", ... */
const char * brew_ui_layout_name(void);

/* ---- controller -> UI ---- */
void brew_ui_set_temp(float temp_f);
void brew_ui_set_action(brew_action_t action);
/** What the controller wants to do and how many seconds until it may (0 = now). While it
 * differs from the action, the status says "Cooling in 4 min" rather than "Holding". */
void brew_ui_set_pending(brew_action_t wanted, int wait_s);
void brew_ui_set_setpoints(float ferment_f, float crash_f);
void brew_ui_set_mode(brew_mode_t mode, bool paused);
void brew_ui_set_units(bool celsius);
void brew_ui_set_batch(const char * name, int day);
/** The temperature is simulated (a demo build): the dial says DEMO. */
void brew_ui_set_demo(bool demo);
/** No valid temperature (no probe, or it stopped answering): the dial shows --.- and NO READING. */
void brew_ui_set_no_reading(void);
/** How the history graph is drawn (default BREW_GRAPH_HAIRLINE). */
void brew_ui_set_graph_style(brew_graph_style_t style);
brew_graph_style_t brew_ui_graph_style(void);
/**
 * Replace the history buffer (oldest first; n may be 0). It holds up to BREW_HIST_MAX samples
 * (1 h at one every 200 ms); the graph stretches whatever it holds to the full width, with the
 * live temperature (brew_ui_set_temp) as the "now" point after the newest sample.
 */
void brew_ui_set_history(const float * temps_f, int n, int ms_per_sample);
/** Append one sample, dropping the oldest when the window is full. */
void brew_ui_push_history(float temp_f);
/** The display's WiFi status (strings are copied). Starting setup brings up its screens. */
void brew_ui_set_wifi(const brew_wifi_t * wifi);
/** Brewfather's state: Settings › Brewfather, and Profile › Brewfather's batches and preview. */
void brew_ui_set_brewfather(const brew_bf_t * bf);
/** The controller's heat/cool settings: differences and temperatures in F, delays in seconds. */
void brew_ui_set_schedule(const brew_schedule_t * s);
void brew_ui_set_tuning(float heat_diff_f, float cool_diff_f, int heat_delay_s, int cool_delay_s, float crash_default_f);

/* ---- input -> UI ---- */
/** Positive = clockwise. Call once per detent, or with the accumulated delta. */
void brew_ui_encoder(int detents);
void brew_ui_press(void);

/* ---- for tests and the harness ---- */
/** The status line as shown: "Cooling to 66°", "Cooling in 4 min", ... */
void brew_ui_status_text(char * buf, size_t n);

#ifdef __cplusplus
}
#endif
