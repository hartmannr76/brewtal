/**
 * brew_layout.h - how a screen layout plugs into brew_ui.
 *
 * A layout draws a brew_model_t. It owns LVGL objects and presentation state (animations,
 * caches) but no product state or rules: those live in the model, so every layout behaves
 * the same and only looks different. Today there is one, brew_layout_round (the dial). A
 * square panel with buttons, or a rectangular one, is another brew_layout_t.
 *
 * Contract:
 *   - create() builds everything on `screen` and draws the model once. Keep `m`; it stays
 *     valid and is only changed by brew_ui between update() calls.
 *   - update() is called after every model change with the BREW_CH_* flags (brew_model.h)
 *     the change returned. React to what you draw; ignore the rest.
 *   - Input never goes through a layout's own logic: route taps / buttons to brew_ui_press()
 *     and brew_ui_encoder(), so the rules stay in the model.
 *   - All calls happen in the LVGL (UI) task.
 */
#pragma once

#include "brew_model.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct brew_layout {
    const char * name;
    void (*create)(lv_obj_t * screen, const brew_model_t * m);
    void (*update)(const brew_model_t * m, uint32_t changes);
} brew_layout_t;

/** The round dial (480 / 360 px round panels with a rotary ring). */
extern const brew_layout_t brew_layout_round;

#ifdef __cplusplus
}
#endif
