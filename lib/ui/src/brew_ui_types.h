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

/** The display's WiFi, as the UI shows it (fed from lib/network's WifiSetup by UiBridge). */
typedef enum {
    BREW_WIFI_NONE = 0,     /* no WiFi on this display (the menu item says so and does nothing) */
    BREW_WIFI_OFF,          /* nothing saved, setup not running */
    BREW_WIFI_SETUP,        /* setup network up, waiting for a phone */
    BREW_WIFI_PHONE,        /* a phone is on the setup network */
    BREW_WIFI_CONNECTING,   /* trying the network picked on the phone */
    BREW_WIFI_CONNECTED,
    BREW_WIFI_OFFLINE,      /* saved network out of reach: retrying in the background */
} brew_wifi_phase_t;

typedef enum {
    BREW_WIFI_ERR_NONE = 0,
    BREW_WIFI_ERR_PASSWORD,
    BREW_WIFI_ERR_NOT_FOUND,
    BREW_WIFI_ERR_FAILED,
} brew_wifi_error_t;

typedef struct {
    brew_wifi_phase_t phase;
    brew_wifi_error_t error;     /* why the last attempt from the phone failed */
    const char * ssid;           /* the network joined, being tried, or saved */
    const char * ap_ssid;        /* the setup network ("BrewDial-4F2A")... */
    const char * ap_password;    /* ...and its password ("malt-7391") */
    const char * address;        /* this display on the joined network ("192.168.1.129"), or "" */
} brew_wifi_t;

/** Brewfather, as Settings › Brewfather shows it (lib/network Brewfather's status). */
typedef enum {
    BREW_BF_NOT_SET_UP = 0,
    BREW_BF_CHECKING,
    BREW_BF_CONNECTED,
    BREW_BF_REJECTED,      /* the user id / API key wasn't accepted */
    BREW_BF_UNREACHABLE,
} brew_bf_account_t;

#define BREW_BF_BATCHES_MAX 8
#define BREW_BF_STEPS_MAX 8   /* = BREW_STEPS_MAX, the schedule's size */

typedef enum {
    BREW_BF_PREVIEW_NONE = 0,
    BREW_BF_PREVIEW_LOADING,
    BREW_BF_PREVIEW_READY,
    BREW_BF_PREVIEW_FAILED,
} brew_bf_preview_t;

/** A Brewfather fermentation step, ready to show. */
typedef struct {
    const char * type;     /* "Primary" */
    float temp_f;
    int ramp_h;            /* 0 = a jump */
    int hold_h;
} brew_bf_step_t;

typedef struct {
    brew_bf_account_t account;
    bool logging;          /* a Custom Stream id is saved */
    bool attached;         /* readings land on a batch... */
    const char * batch;    /* ...this one: "#18 Pale Ale" */
    int last_log_min;      /* minutes since Brewfather last recorded a reading, -1 unknown */
    /* import (Profile › Brewfather) */
    int nbatches;
    const char * batches[BREW_BF_BATCHES_MAX];   /* the Fermenting batches: "#18 Pale Ale" */
    brew_bf_preview_t preview;   /* the batch picked from them */
    const char * preview_label;
    int preview_total;     /* steps in Brewfather (only the first BREW_BF_STEPS_MAX are imported) */
    int preview_n;
    brew_bf_step_t preview_steps[BREW_BF_STEPS_MAX];
    int today_day;         /* the batch's day today (1 = its first), -1: no start date */
    int today_step;        /* the step running today (0-based), -1: none (not started / past the last) */
} brew_bf_t;

/** Controller settings the user can change from Settings › Heat / Cool. */
typedef enum {
    BREW_TUNE_HEAT_DIFF = 0,    /* degrees F below the target where heating starts (a difference) */
    BREW_TUNE_COOL_DIFF,        /* degrees F above the target where cooling starts */
    BREW_TUNE_HEAT_DELAY,       /* seconds heat rests after switching off */
    BREW_TUNE_COOL_DELAY,       /* seconds cooling rests after switching off (the compressor) */
    BREW_TUNE_CRASH,            /* degrees F: where Cold crash starts */
    BREW_TUNE_N,
} brew_tuning_t;

/** How the Ferment target is set. */
typedef enum { BREW_PROFILE_CONSTANT = 0, BREW_PROFILE_SCHEDULE = 1 } brew_profile_t;

#define BREW_STEPS_MAX 8
#define BREW_STEP_DONE 255   /* brew_schedule_t.step once the last step has finished */

/** One schedule step: ramp to temp_f over ramp_h hours (0 = jump), then hold it hold_h hours. */
typedef struct {
    float temp_f;
    int ramp_h;
    int hold_h;
} brew_step_t;

typedef struct {
    brew_profile_t profile;
    int step;             /* the running step (0-based), or BREW_STEP_DONE */
    bool override_on;     /* the dial set the target until the next step */
    int nsteps;
    brew_step_t steps[BREW_STEPS_MAX];
} brew_schedule_t;

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
    /** User picked "Reset day count": Day 1 starts now. */
    void (*on_reset_day)(void * user);
    /** User picked WiFi in the menu: open WiFi setup. */
    void (*on_wifi_setup)(void * user);
    /** User pressed while WiFi setup showed: close it, change nothing. */
    void (*on_wifi_cancel)(void * user);
    /** User picked Forget network: drop the saved WiFi (setup opens again). */
    void (*on_wifi_forget)(void * user);
    /** User saved a value in Settings › Heat / Cool. Temperatures and differences in F, delays in s. */
    void (*on_tuning)(brew_tuning_t what, float value, void * user);
    /** User picked a profile (Constant, or Use this schedule / Stop schedule). */
    void (*on_profile)(brew_profile_t profile, void * user);
    /** User changed schedule step `i` (0-based). */
    void (*on_step)(int i, float temp_f, int ramp_h, int hold_h, void * user);
    /** User added or removed a step: the schedule now has `n`. */
    void (*on_step_count)(int n, void * user);
    /** User picked Check now in Settings › Brewfather (or Refresh in Profile › Brewfather). */
    void (*on_bf_check)(void * user);
    /** User picked Fermenting batch `i` in Profile › Brewfather: fetch it for a preview. */
    void (*on_bf_pick)(int i, void * user);
    /** User picked Run this profile: import the previewed batch into the schedule. */
    void (*on_bf_run)(void * user);
    void * user;
} brew_ui_cbs_t;

#ifdef __cplusplus
}
#endif
