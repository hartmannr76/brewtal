/**
 * brew_ui_types.h - the plain types of the brew_ui API (no LVGL), shared by the public API
 * (brew_ui.h), the UI model (brew_model.h) and the layouts.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BREW_MODE_FERMENT = 0,
    BREW_MODE_CRASH   = 1,
} brew_mode_t;

typedef enum {
    BREW_ACTION_IDLE = 0,   /* within deadband -> "Holding at" */
    BREW_ACTION_COOLING,
    BREW_ACTION_HEATING,
} brew_action_t;

/** How the history graph behind the reading is drawn. Presentation only: same data either way. */
typedef enum {
    BREW_GRAPH_HAIRLINE = 0,   /* a thin line fading in from the left, no fill (default) */
    BREW_GRAPH_MIST,           /* line with a soft glow over a fill that fades into the background */
    BREW_GRAPH_STYLE_N,
} brew_graph_style_t;

typedef struct {
    /** User committed a new target (turned the crown, then pressed or waited). */
    void (*on_setpoint)(brew_mode_t mode, float temp_f, void * user);
    /** User picked Ferment / Cold crash / Pause / Resume in the menu. */
    void (*on_mode)(brew_mode_t mode, bool paused, void * user);
    /** User toggled display units. */
    void (*on_units)(bool celsius, void * user);
    void * user;
} brew_ui_cbs_t;

#ifdef __cplusplus
}
#endif
