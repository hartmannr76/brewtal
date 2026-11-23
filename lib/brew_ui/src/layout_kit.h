/**
 * layout_kit.h - drawing pieces shared by the layouts (layout_*.c). Internal to brew_ui.
 *
 * Everything here is presentation: the state palette, the rolling-digit number view,
 * change-guarded LVGL setters, the cross-fading state background and the history graph.
 * A layout composes these with its own geometry; rules stay in the model (brew_model.h).
 */
#pragma once

#include "brew_layout.h"
#include "brew_ui.h"
#include "brew_bg.h"
#include "fonts/brew_fonts.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL 9.1/9.2 compatibility (Elecrow ships 9.1 with its Arduino examples) */
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3
#define lv_anim_set_reverse_duration lv_anim_set_playback_duration
#endif

#ifndef BREW_UI_ALLOC
#if defined(ESP_PLATFORM)
/* large canvases: PSRAM when present, internal RAM otherwise */
#include "esp_heap_caps.h"
#define BREW_UI_ALLOC(sz) heap_caps_malloc_prefer((sz), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)
#define BREW_UI_FREE(p)   heap_caps_free(p)
#else
#define BREW_UI_ALLOC(sz) lv_malloc(sz)
#define BREW_UI_FREE(p)   lv_free(p)
#endif
#endif
#ifndef BREW_UI_FREE
#define BREW_UI_FREE(p) lv_free(p)
#endif

#ifndef BREW_FADE_MS
#define BREW_FADE_MS 160          /* view / ring / graph-dim fades */
#endif
#ifndef BREW_BG_FADE_MS
#define BREW_BG_FADE_MS 350       /* background colour cross-fade between states */
#endif

#define KIT_DISP      BREW_UI_DISP_SIZE
#define KIT_COOL_TEXT 0x7cd4ff    /* cooling status text and big digits */

/** Line / accent colour per visual state (graph, ring, gauge, tinted numerals): the harness's --s-hi. */
extern const uint32_t kit_line[BREW_VS_N];
/** The state colour itself (the harness's --s): pill fill, the light behind the glass. */
extern const uint32_t kit_base[BREW_VS_N];

/* ---- change-guarded setters --------------------------------------------------------------
 * LVGL invalidates (and re-renders) an object's whole area on every style/text set, even
 * when the value is unchanged, so refreshes only touch what actually changed. */
bool kit_col_eq(lv_color_t a, lv_color_t b);
void kit_set_text(lv_obj_t * l, const char * t);
void kit_set_text_color(lv_obj_t * o, lv_color_t c);
void kit_set_text_opa(lv_obj_t * o, lv_opa_t v);
void kit_set_bg_color(lv_obj_t * o, lv_color_t c);
void kit_set_font(lv_obj_t * o, const lv_font_t * f);
void kit_set_hidden(lv_obj_t * o, bool h);

/** An unstyled, non-clickable, non-scrolling object. */
lv_obj_t * kit_plain(lv_obj_t * parent);
lv_obj_t * kit_canvas(lv_obj_t * parent, void * buf, int32_t w, int32_t h, lv_color_format_t cf);

void kit_opa_exec(void * o, int32_t v);
/** Animate an object's opacity to `to` (unhides it first when fading in). */
void kit_fade_to(lv_obj_t * o, lv_opa_t to, uint32_t ms);
/** Slow endless pulse (the status dot). */
void kit_breathe(lv_obj_t * o);

/* ---- number view: one fixed-width cell per character, so only the digits that change
 * animate (slide + fade), iOS numericText style. ----------------------------------------- */
#define KIT_NV_MAX 6

typedef struct {
    lv_obj_t * box;
    lv_obj_t * cell[KIT_NV_MAX];
    lv_obj_t * lbl[KIT_NV_MAX];
    lv_obj_t * deg;
    const lv_font_t * font;
    int32_t digit_w, dot_w, minus_w;
    char cur[KIT_NV_MAX + 1];
    lv_color_t color;
    bool color_set;
} kit_numview_t;

void kit_nv_create(kit_numview_t * nv, lv_obj_t * parent, const lv_font_t * font, const lv_font_t * deg_font,
                   int32_t deg_dy, int32_t deg_pad);
void kit_nv_set_color(kit_numview_t * nv, lv_color_t c);
/** The text after the digits (default "°"), e.g. "°F". */
void kit_nv_set_unit(kit_numview_t * nv, const char * unit);
void kit_nv_set(kit_numview_t * nv, const char * str, bool animate);

/* ---- background: the smoked-glass face per state (brew_bg_render_glass), cross-faded ---- */
typedef struct {
    lv_obj_t * bg[2];
    int front;
    int gid;      /* gradient shown, -1 = none */
} kit_bg_t;

/** Two full-screen canvases at (0,0) of `parent`. Pre-renders every gradient (shared by all
 * layouts, kept for the life of the program) so a state change never stalls. */
void kit_bg_create(kit_bg_t * b, lv_obj_t * parent);
void kit_bg_show(kit_bg_t * b, brew_vstate_t vs, bool animate);

/* ---- history graph ------------------------------------------------------------------------
 * Drawn in the model's graph_style: BREW_GRAPH_HAIRLINE (a thin line, no fill) or
 * BREW_GRAPH_MIST (the line with a soft glow over a fill fading into the background).
 * Whatever is buffered, stretched to the full width: the oldest sample at x0, the live reading
 * (the "now" dot) at x1, evenly spaced between. A few seconds of data already fill the graph;
 * as samples arrive they squeeze together until the window is full (BREW_HIST_MAX samples),
 * then the oldest drops off and the curve scrolls left.
 * Graph index i (0..brew_model_hist_last()) is sample i, or the live reading when i == hist_n.
 * More samples than KIT_GRAPH_PTS are averaged into that many points for drawing (a 1 h window
 * is 18000 samples, the plot ~340 px wide); the live reading is always the last point as is.
 */
#define KIT_GRAPH_PTS  181
#define KIT_POLY_STEPS 8     /* smoothing steps per segment, fewer when points are close */
#define KIT_POLY_MAX   961   /* outline budget: 8 steps up to 121 points, 5 at 181 */
/* The live point redraws at most this often (a full graph render; cheap enough on the S3 at
 * this rate, and only when the point moved >= 1 px). */
#define KIT_GRAPH_REDRAW_MS 200   /* a sample every 200 ms: each one is drawn */
/* The line (and Mist's fill) fade in from nothing at x0 to full over this share of the width,
 * so the window has no hard left edge. */
/* Drawn points are smoothed with a triangular window this many points either side, so the
 * probe's 0.1 F steps read as gentle slopes, not a staircase. The live point stays exact. */
#define KIT_GRAPH_SMOOTH 6
/* The y axis is fixed to the ring's scale, BREW_T_MIN..BREW_T_MAX (30..80 F): it never moves,
 * whatever the readings or the target do. Readings outside it ride the edge. */
#define KIT_GRAPH_FADE_HAIRLINE 0.50f
#define KIT_GRAPH_FADE_MIST     0.42f

typedef struct {
    /* geometry: fill in before kit_graph_create(). Everything is in the parent's coordinates. */
    int32_t cx, cy, cw, ch;       /* the canvas */
    float x0, x1;                 /* oldest sample .. "now" */
    float yt, yb;                 /* plot top / bottom */
    float line_w;
    int32_t halo_d, dot_d;        /* "now" / scrub marker */
    float clip_cx, clip_cy, clip_r;   /* round glass: nothing drawn outside it (clip_r 0 = no clip) */

    lv_obj_t * canvas;
    lv_obj_t * halo;
    lv_obj_t * dot;
    uint8_t * buf;
    brew_pt_t pts[KIT_GRAPH_PTS];   /* drawn points (bucketed samples), then the live reading */
    int npts;
    int nsrc;                       /* graph indices behind them: samples + live */
    brew_pt_t poly[KIT_POLY_MAX];
    int npoly;
    float g_min, g_max;
    int32_t rows0, rows1;           /* canvas rows the last render drew in (cleared/redrawn next time) */
    brew_pt_t drawn[KIT_GRAPH_PTS];   /* what the canvas shows now: skip renders that wouldn't change it */
    int ndrawn;
    uint32_t drawn_lc;
    int drawn_style;
    float drawn_sy;
    bool dirty;
    uint32_t last_ms;
} kit_graph_t;

/** Creates the canvas, then the halo and dot above it, on `parent`. */
void kit_graph_create(kit_graph_t * g, lv_obj_t * parent);
void kit_graph_render(kit_graph_t * g, const brew_model_t * m);
/** Puts the marker on the live point, or on the scrub position in the history view. */
void kit_graph_place_marker(kit_graph_t * g, const brew_model_t * m);
/** The marker's graph index (clamped), or -1 when there is nothing drawn. */
int kit_graph_marker_index(const kit_graph_t * g, const brew_model_t * m);
/** x of graph index i (as drawn at the last render). */
float kit_graph_index_x(const kit_graph_t * g, int i);
float kit_graph_temp_y(const kit_graph_t * g, float temp_f);
/** True if the live reading moved enough to be worth redrawing (>= 1 px or off scale). */
bool kit_graph_live_moved(const kit_graph_t * g, float temp_f);
/** Frees the canvas buffer; the objects go with their parent. */
void kit_graph_free(kit_graph_t * g);

#ifdef __cplusplus
}
#endif
