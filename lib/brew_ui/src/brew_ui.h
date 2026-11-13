/**
 * brew_ui.h - round fermentation controller UI (LVGL 9).
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

/* ---- controller -> UI ---- */
void brew_ui_set_temp(float temp_f);
void brew_ui_set_action(brew_action_t action);
void brew_ui_set_setpoints(float ferment_f, float crash_f);
void brew_ui_set_mode(brew_mode_t mode, bool paused);
void brew_ui_set_units(bool celsius);
void brew_ui_set_batch(const char * name, int day);
/**
 * Replace the history buffer (oldest first; n may be 0). The background graph is a fixed
 * window of 120 samples that fills from the left; the live temperature (brew_ui_set_temp) is
 * drawn as the "now" point after the newest sample.
 */
void brew_ui_set_history(const float * temps_f, int n, int minutes_per_sample);
/** Append one sample, dropping the oldest when the window is full. */
void brew_ui_push_history(float temp_f);

/* ---- input -> UI ---- */
/** Positive = clockwise. Call once per detent, or with the accumulated delta. */
void brew_ui_encoder(int detents);
void brew_ui_press(void);

#ifdef __cplusplus
}
#endif
