/**
 * brew_model.h - what the BrewDial UI knows and decides, with no drawing in it.
 *
 * The model holds the controller's state (temperature, targets, mode, history), the
 * interaction state (which view, the target being adjusted, the menu and scrub position) and
 * the rules (what a turn or a press does, when an adjusted target is committed). It knows
 * nothing about LVGL, fonts, geometry or screen shape, so every layout (round dial, square
 * panel with buttons, ...) shares it, and it is unit-tested on the host.
 *
 * Every mutating call returns BREW_CH_* flags saying what changed. The layout turns those into
 * drawing; brew_ui.c (the glue) also uses them to run the auto-commit timer.
 *
 * Temperatures are degrees Fahrenheit, as in the brew_ui API.
 */
#pragma once

#include "brew_ui_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BREW_HIST_MAX  3600     /* history window in samples: 2 h at one every 2 s */
#define BREW_SCRUB_S   60       /* the history view moves this far back/forward per step */
#define BREW_T_MIN     30.0f    /* adjustable target range, F */
#define BREW_T_MAX     80.0f
#define BREW_COMMIT_MS 2800     /* an adjusted target commits itself after this long */

/** Visual state: picks colours. Same rules everywhere (the web harness mirrors them). */
typedef enum { BREW_VS_COOL, BREW_VS_IDLE, BREW_VS_HEAT, BREW_VS_CRASH, BREW_VS_PAUSE, BREW_VS_N } brew_vstate_t;

typedef enum { BREW_VIEW_HOME, BREW_VIEW_MENU, BREW_VIEW_HIST } brew_view_t;

typedef enum {
    BREW_MENU_FERMENT, BREW_MENU_CRASH, BREW_MENU_PAUSE, BREW_MENU_HISTORY, BREW_MENU_UNITS, BREW_MENU_N
} brew_menu_item_t;

/* What changed (returned by every mutating call). */
enum {
    BREW_CH_REFRESH       = 1u << 0,  /* something visible changed: redraw */
    BREW_CH_ANIMATE       = 1u << 1,  /* ...and it may animate (digit roll, fades) */
    BREW_CH_TEMP          = 1u << 2,  /* the live temperature changed (layouts may throttle its graph point) */
    BREW_CH_GRAPH         = 1u << 3,  /* graph data or scale changed: redraw the graph with this refresh */
    BREW_CH_GRAPH_LAZY    = 1u << 4,  /* a sample was appended: redraw the graph at the layout's own pace */
    BREW_CH_VIEW          = 1u << 5,  /* switched view (m->view): reset per-view presentation state */
    BREW_CH_MENU_MOVE     = 1u << 6,  /* the menu selection moved (m->menu_idx) */
    BREW_CH_ARM_COMMIT    = 1u << 7,  /* (re)start the BREW_COMMIT_MS countdown, then call brew_model_commit() */
    BREW_CH_DISARM_COMMIT = 1u << 8,  /* stop the countdown: nothing is being adjusted */
};

typedef struct {
    brew_ui_cbs_t cbs;

    /* from the controller */
    float temp_f;
    bool temp_known;          /* a reading has arrived */
    float sp_f[2];            /* target per mode */
    brew_mode_t mode;
    bool paused;
    brew_action_t action;
    brew_action_t wanted;     /* what the controller wants to do; differs from action while held off */
    int wait_min;             /* ...and in how many minutes it may (rounded up, >= 1 while waiting) */
    bool celsius;             /* display units */
    float hist[BREW_HIST_MAX];
    int hist_n;
    int hist_sec_per;         /* seconds per history sample */
    char batch[32];
    int day;

    /* interaction */
    brew_view_t view;
    bool setting;             /* adjusting the target (home view) */
    float draft_f;            /* the target being adjusted */
    int menu_idx;
    int hist_idx;             /* scrub position: 0..brew_model_hist_last() */
} brew_model_t;

void brew_model_init(brew_model_t * m, const brew_ui_cbs_t * cbs);

/* ---- controller -> model ---- */
uint32_t brew_model_set_temp(brew_model_t * m, float temp_f);
uint32_t brew_model_set_action(brew_model_t * m, brew_action_t a);
/** What the controller wants and how long until it may start it (restart delay etc.).
 * Changes are reported only when the minute shown changes. */
uint32_t brew_model_set_pending(brew_model_t * m, brew_action_t wanted, int wait_s);
uint32_t brew_model_set_setpoints(brew_model_t * m, float ferment_f, float crash_f);
uint32_t brew_model_set_mode(brew_model_t * m, brew_mode_t mode, bool paused);
uint32_t brew_model_set_units(brew_model_t * m, bool celsius);
uint32_t brew_model_set_batch(brew_model_t * m, const char * name, int day);
uint32_t brew_model_set_history(brew_model_t * m, const float * temps_f, int n, int seconds_per_sample);
uint32_t brew_model_push_history(brew_model_t * m, float temp_f);

/* ---- input -> model ---- */
/** Positive = clockwise / up / warmer. One call per detent or button press, or a sum. */
uint32_t brew_model_encoder(brew_model_t * m, int steps);
/** The knob press, a screen tap, or the select button. */
uint32_t brew_model_press(brew_model_t * m);
/** Commit the target being adjusted (the auto-commit countdown expired). No-op otherwise. */
uint32_t brew_model_commit(brew_model_t * m);

/* ---- derived (read-only) ---- */
brew_vstate_t brew_model_vstate(const brew_model_t * m);
/** The target currently shown: the draft while adjusting, else the active mode's target. */
float brew_model_shown_target(const brew_model_t * m);
/** In display units (F or C). */
float brew_model_to_disp(const brew_model_t * m, float temp_f);
/** "68.4" in display units. */
void brew_model_fmt_temp(const brew_model_t * m, char * buf, size_t n, float temp_f);
/** The controller wants to cool or heat but is held off (restart delay, changeover gap). */
bool brew_model_waiting(const brew_model_t * m);
/** "Cooling to 66°", "Holding at 66°", "Heating to 66°", "Crashing to 34°", "Paused";
 * while waiting: "Cooling in 4 min", "Heating in 9 min", "Crashing in 4 min". */
void brew_model_status_text(const brew_model_t * m, char * buf, size_t n);
/** "Pale ale · Day 4", or "Pale ale · Cold crash". */
void brew_model_batch_text(const brew_model_t * m, char * buf, size_t n);

/* History: index i in 0..brew_model_hist_last() is sample i (oldest first), or the live
 * reading when i == hist_n. -1 = nothing to show yet. */
int brew_model_hist_last(const brew_model_t * m);
float brew_model_hist_val(const brew_model_t * m, int i);
/** "Now", "2:41 PM" (wall clock set), "40 s ago" or "1h 05m ago". */
void brew_model_hist_time(const brew_model_t * m, int i, char * buf, size_t n);
/** Samples per history-view step (BREW_SCRUB_S worth, at least 1). */
int brew_model_hist_step(const brew_model_t * m);
/** How far back the history window reaches: "last 2 hours", "last hour", "last 45 min". */
void brew_model_hist_window_text(const brew_model_t * m, char * buf, size_t n);

/* Menu */
/** Label for item i ("Pause" / "Resume", "Units · °F", ...). */
void brew_model_menu_label(const brew_model_t * m, int i, char * buf, size_t n);
/** Item i is the mode currently running (shows a marker). */
bool brew_model_menu_active(const brew_model_t * m, int i);

/* Key hints (layouts that label their keys, like the square panel) */
/** What a press does right now, in one short word: "Menu", "Set", "Start", "Pause",
 * "Resume", "Open", "Switch", "Close", "Back". */
const char * brew_model_press_hint(const brew_model_t * m);
/** Would brew_model_encoder(m, dir) change anything? (false at the end of a range) */
bool brew_model_can_step(const brew_model_t * m, int dir);

#ifdef __cplusplus
}
#endif
