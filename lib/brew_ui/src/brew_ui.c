/**
 * brew_ui.c - round fermentation controller UI (LVGL 9.3)
 *
 * Layer stack (bottom -> top):
 *   bg[0], bg[1]   RGB565 canvases, dithered radial gradient; cross-faded on state change
 *   graph          ARGB8888 canvas with the area chart + line (dimmable)
 *   halo, dot      "now"/scrub marker on the curve
 *   ticks          lv_scale, only visible while adjusting
 *   arc, knob      setpoint ring
 *   v_home / v_menu / v_hist   content views, cross-faded
 *
 * Geometry is authored in the mockup's units and scaled:
 *   DS(x)  SVG viewBox units (240 = full diameter)
 *   DC(x)  CSS px of the mockup screen (276 = full diameter)
 */
#include "brew_ui.h"
#include "brew_bg.h"
#include "fonts/brew_fonts.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* LVGL 9.1/9.2 compatibility (Elecrow ships 9.1 with its Arduino examples) */
#if LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR < 3
#define lv_anim_set_reverse_duration lv_anim_set_playback_duration
#endif

#ifndef BREW_UI_ALLOC
#if defined(ESP_PLATFORM)
/* ~720 KB of canvases: PSRAM when present, internal RAM otherwise */
#include "esp_heap_caps.h"
#define BREW_UI_ALLOC(sz) heap_caps_malloc_prefer((sz), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT)
#else
#define BREW_UI_ALLOC(sz) lv_malloc(sz)
#endif
#endif

#define DISP      BREW_UI_DISP_SIZE
#define DS(x)     ((float)(x) * (float)DISP / 240.0f)
#define DC(x)     ((int32_t)lroundf((float)(x) * (float)DISP / 276.0f))

#define T_MIN     30.0f
#define T_MAX     80.0f
#define A0        135.0f
#define SWEEP     270.0f
#define RING_R    DS(113)
#define HIST_MAX  120                      /* graph window in samples: 2 h at 1 per minute */
#define POLY_STEPS 8
#define G_Y0      ((int32_t)DS(146))       /* top of graph canvas */
#define G_H       (DISP - G_Y0)
#define G_X0      DS(36)                   /* oldest sample: left end, inside the round glass */
#define G_X1      DS(204)                  /* right end: "now" once the window is full */
#define G_YT      DS(158)
#define G_YB      DS(212)

#define COMMIT_MS 2800
#ifndef BREW_FADE_MS
#define BREW_FADE_MS 160          /* view / ring / graph-dim fades */
#endif
#ifndef BREW_BG_FADE_MS
#define BREW_BG_FADE_MS 350       /* background colour cross-fade between states */
#endif
#define GRAPH_MIN_REDRAW_MS 10000 /* live-temperature graph updates at most this often */
#define COOL_TEXT 0x7cd4ff

typedef enum { VS_COOL, VS_IDLE, VS_HEAT, VS_CRASH, VS_PAUSE, VS_N } vstate_t;
typedef enum { VIEW_HOME, VIEW_MENU, VIEW_HIST } view_t;

static const brew_grad_t s_grad[VS_N] = {
    {{0x3070b0, 0x20456b, 0x1c2a37, 0x171f27}, {0.0f, 0.42f, 0.78f, 1.0f}}, /* cool  */
    {{0x3070b0, 0x20456b, 0x1c2a37, 0x171f27}, {0.0f, 0.42f, 0.78f, 1.0f}}, /* idle  */
    {{0xff8f3a, 0xc4561a, 0x4a2516, 0x2a1912}, {0.0f, 0.40f, 0.78f, 1.0f}}, /* heat  */
    {{0x8c5ff2, 0x4b2e98, 0x241c3e, 0x18142a}, {0.0f, 0.42f, 0.78f, 1.0f}}, /* crash */
    {{0x5f6871, 0x383f46, 0x22272c, 0x1a1d21}, {0.0f, 0.45f, 0.80f, 1.0f}}, /* pause */
};
static const uint32_t s_line[VS_N] = { 0x7cd4ff, 0x7cd4ff, 0xffcf8a, 0xd8bcff, 0xc3cad1 };

/* ------------------------------------------------------------------ */
/* Number view: one fixed-width cell per character, so only the       */
/* digits that change animate (slide + fade), iOS numericText style.  */
/* ------------------------------------------------------------------ */
#define NV_MAX 6

typedef struct {
    lv_obj_t * box;
    lv_obj_t * cell[NV_MAX];
    lv_obj_t * lbl[NV_MAX];
    lv_obj_t * deg;
    const lv_font_t * font;
    int32_t digit_w, dot_w, minus_w;
    char cur[NV_MAX + 1];
    lv_color_t color;
    bool color_set;
} numview_t;

static bool col_eq(lv_color_t a, lv_color_t b);
static void ty_exec(void * o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }
static void tx_opa_exec(void * o, int32_t v) { lv_obj_set_style_text_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static int32_t glyph_w(const lv_font_t * f, uint32_t c)
{
    return (int32_t)lv_font_get_glyph_width(f, c, 0);
}

static void nv_create(numview_t * nv, lv_obj_t * parent, const lv_font_t * font,
                      const lv_font_t * deg_font, int32_t deg_dy)
{
    memset(nv, 0, sizeof(*nv));
    nv->font = font;
    const int32_t kern = -(int32_t)lroundf(0.04f * (float)font->line_height / 1.21f);
    for(char c = '0'; c <= '9'; c++) {
        int32_t w = glyph_w(font, (uint32_t)c);
        if(w > nv->digit_w) nv->digit_w = w;
    }
    nv->digit_w += kern;
    nv->dot_w   = glyph_w(font, '.') + kern;
    nv->minus_w = glyph_w(font, '-') + kern;

    nv->box = lv_obj_create(parent);
    lv_obj_remove_style_all(nv->box);
    lv_obj_set_size(nv->box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nv->box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nv->box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(nv->box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    for(int i = 0; i < NV_MAX; i++) {
        lv_obj_t * cell = lv_obj_create(nv->box);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, nv->digit_w, font->line_height);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_OVERFLOW_VISIBLE | LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t * l = lv_label_create(cell);
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
        lv_label_set_text(l, "");
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 0);
        nv->cell[i] = cell;
        nv->lbl[i] = l;
    }

    nv->deg = lv_label_create(nv->box);
    lv_obj_set_style_text_font(nv->deg, deg_font, 0);
    lv_obj_set_style_text_color(nv->deg, lv_color_white(), 0);
    lv_obj_set_style_translate_y(nv->deg, deg_dy, 0);
    lv_obj_set_style_pad_left(nv->deg, DC(2), 0);
    lv_label_set_text(nv->deg, "\xC2\xB0");
}

static void nv_set_color(numview_t * nv, lv_color_t c)
{
    if(nv->color_set && col_eq(nv->color, c)) return;
    nv->color = c;
    nv->color_set = true;
    for(int i = 0; i < NV_MAX; i++) lv_obj_set_style_text_color(nv->lbl[i], c, 0);
    lv_obj_set_style_text_color(nv->deg, c, 0);
}

static void nv_anim_in(numview_t * nv, lv_obj_t * l, bool up)
{
    const int32_t dy = (int32_t)lroundf(0.45f * (float)nv->font->line_height / 1.21f);
    lv_anim_delete(l, ty_exec);
    lv_anim_delete(l, tx_opa_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, l);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, ty_exec);
    lv_anim_set_values(&a, up ? dy : -dy, 0);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, tx_opa_exec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_start(&a);
}

static void nv_set(numview_t * nv, const char * str, bool animate)
{
    if(strcmp(nv->cur, str) == 0) return;
    const bool up = atof(str) >= atof(nv->cur);
    const int L = (int)strlen(str), Lo = (int)strlen(nv->cur);
    const bool had_old = Lo > 0;

    for(int k = 0; k < NV_MAX; k++) {
        const int si = L - NV_MAX + k;   /* right-aligned slot mapping */
        const int oi = Lo - NV_MAX + k;
        if(si < 0) {
            lv_obj_add_flag(nv->cell[k], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const char c = str[si];
        const char oc = oi >= 0 ? nv->cur[oi] : 0;
        if(c == oc && lv_obj_has_flag(nv->cell[k], LV_OBJ_FLAG_HIDDEN) == false) continue; /* unchanged */
        lv_obj_remove_flag(nv->cell[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(nv->cell[k], c == '.' ? nv->dot_w : (c == '-' ? nv->minus_w : nv->digit_w));
        char t[2] = { c, 0 };
        lv_label_set_text(nv->lbl[k], t);
        if(animate && had_old && c != oc) nv_anim_in(nv, nv->lbl[k], up);
    }
    strncpy(nv->cur, str, NV_MAX);
    nv->cur[NV_MAX] = 0;
}

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */
static struct {
    brew_ui_cbs_t cbs;

    /* model (from controller) */
    float temp_f;
    bool temp_known;      /* a reading has arrived (the live point on the graph) */
    float sp_f[2];
    brew_mode_t mode;
    bool paused;
    brew_action_t action;
    bool celsius;
    float hist[HIST_MAX];
    int hist_n;
    int hist_min_per;
    char batch[32];
    int day;

    /* interaction */
    view_t view;
    bool setting;
    float draft_f;
    int menu_idx;
    int hist_idx;
    vstate_t vs;
    bool vs_valid;

    /* objects */
    lv_obj_t * root;
    lv_obj_t * bg[2];
    uint16_t * bg_cache[4];  /* one pre-rendered background per distinct gradient */
    int bg_front;
    int bg_gid;              /* gradient currently shown, -1 = none */
    lv_obj_t * graph;
    uint8_t * graph_buf;
    lv_obj_t * halo;
    lv_obj_t * dot;
    brew_pt_t pts[HIST_MAX + 1];   /* samples, then the live reading */
    int npts;
    brew_pt_t poly[HIST_MAX * POLY_STEPS + 1];
    int npoly;
    float g_min, g_max;
    bool graph_dirty;
    uint32_t graph_last_ms;

    lv_obj_t * ticks;
    lv_obj_t * arc;
    lv_obj_t * knob;

    lv_obj_t * v_home;
    lv_obj_t * v_menu;
    lv_obj_t * v_hist;
    lv_obj_t * st_dot;
    lv_obj_t * st_lbl;
    lv_obj_t * sub_lbl;
    numview_t big;
    numview_t hval;
    lv_obj_t * htime;

    lv_obj_t * item[5];
    lv_obj_t * item_ico[5];
    lv_obj_t * item_txt[5];
    lv_obj_t * item_dot[5];
    int32_t wheel_pos; /* menu index * 256, animated */

    lv_timer_t * commit_t;
    lv_timer_t * graph_t;
} U;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */
static float to_disp(float f) { return U.celsius ? (f - 32.0f) * 5.0f / 9.0f : f; }

static float frac_t(float f)
{
    float v = (f - T_MIN) / (T_MAX - T_MIN);
    return v < 0.002f ? 0.002f : (v > 0.998f ? 0.998f : v);
}

static float norm_deg(float a)
{
    while(a >= 360.0f) a -= 360.0f;
    while(a < 0.0f) a += 360.0f;
    return a;
}

static void opa_exec(void * o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static void fade_to(lv_obj_t * o, lv_opa_t to, uint32_t ms)
{
    lv_anim_delete(o, opa_exec);
    lv_opa_t from = lv_obj_get_style_opa(o, 0);
    if(to > 0 && lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    if(from == to) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* Change-guarded setters. LVGL invalidates (and re-renders) an object's whole area on
 * every style/text set, even when the value is unchanged. On the ring that's ~the
 * entire screen, so refresh() must only touch what actually changed. */
static bool col_eq(lv_color_t a, lv_color_t b) { return a.red == b.red && a.green == b.green && a.blue == b.blue; }
static void set_text(lv_obj_t * l, const char * t)
{
    const char * c = lv_label_get_text(l);
    if(c && strcmp(c, t) == 0) return;
    lv_label_set_text(l, t);
}
static void set_text_color(lv_obj_t * o, lv_color_t c)
{
    if(!col_eq(lv_obj_get_style_text_color(o, 0), c)) lv_obj_set_style_text_color(o, c, 0);
}
static void set_text_opa(lv_obj_t * o, lv_opa_t v)
{
    if(lv_obj_get_style_text_opa(o, 0) != v) lv_obj_set_style_text_opa(o, v, 0);
}
static void set_bg_color(lv_obj_t * o, lv_color_t c)
{
    if(!col_eq(lv_obj_get_style_bg_color(o, 0), c)) lv_obj_set_style_bg_color(o, c, 0);
}
static void set_font(lv_obj_t * o, const lv_font_t * f)
{
    if(lv_obj_get_style_text_font(o, 0) != f) lv_obj_set_style_text_font(o, f, 0);
}
static void set_hidden(lv_obj_t * o, bool h)
{
    if(lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == h) return;
    if(h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t * plain_obj(lv_obj_t * parent)
{
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static vstate_t compute_vs(void)
{
    if(U.paused) return VS_PAUSE;
    if(U.mode == BREW_MODE_CRASH) return VS_CRASH;
    if(U.action == BREW_ACTION_COOLING) return VS_COOL;
    if(U.action == BREW_ACTION_HEATING) return VS_HEAT;
    return VS_IDLE;
}

/* ------------------------------------------------------------------ */
/* Background: pre-rendered per gradient, cross-faded on change       */
/* ------------------------------------------------------------------ */
static int grad_id(vstate_t vs)
{
    switch(vs) {
        case VS_HEAT:  return 1;
        case VS_CRASH: return 2;
        case VS_PAUSE: return 3;
        default:       return 0;   /* cooling and holding share the blue gradient */
    }
}
static const vstate_t s_gid_vs[4] = { VS_COOL, VS_HEAT, VS_CRASH, VS_PAUSE };

static uint16_t * bg_get(int gid)
{
    if(!U.bg_cache[gid]) {
        U.bg_cache[gid] = BREW_UI_ALLOC((size_t)DISP * DISP * 2);
        LV_ASSERT_MALLOC(U.bg_cache[gid]);
        brew_bg_render(U.bg_cache[gid], DISP, DISP, &s_grad[s_gid_vs[gid]]);
    }
    return U.bg_cache[gid];
}
static void bg_fade_done(lv_anim_t * a)
{
    LV_UNUSED(a);
    const int nf = 1 - U.bg_front;
    lv_obj_set_style_opa(U.bg[U.bg_front], LV_OPA_TRANSP, 0);
    lv_obj_move_to_index(U.bg[nf], 0);
    lv_obj_move_to_index(U.bg[U.bg_front], 1);
    U.bg_front = nf;
}

static void bg_show(vstate_t vs, bool animate)
{
    const int gid = grad_id(vs);
    if(gid == U.bg_gid) return;            /* same gradient: nothing to redraw */
    const int back = 1 - U.bg_front;
    /* finish an in-flight fade first */
    if(lv_anim_get(U.bg[back], opa_exec)) {
        lv_anim_delete(U.bg[back], opa_exec);
        lv_obj_set_style_opa(U.bg[back], LV_OPA_COVER, 0);
        bg_fade_done(NULL);
        bg_show(vs, animate);
        return;
    }
    U.bg_gid = gid;
    if(!animate) {
        lv_canvas_set_buffer(U.bg[U.bg_front], bg_get(gid), DISP, DISP, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(U.bg[U.bg_front]);
        return;
    }
    lv_canvas_set_buffer(U.bg[back], bg_get(gid), DISP, DISP, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(U.bg[back]);
    lv_obj_set_style_opa(U.bg[back], LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, U.bg[back]);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, BREW_BG_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, bg_fade_done);
    lv_anim_start(&a);
}

/* ------------------------------------------------------------------ */
/* Graph                                                              */
/* ------------------------------------------------------------------ */
/*
 * The graph is a fixed time window of HIST_MAX sample slots, oldest on the left. It starts
 * empty and fills left to right: sample i sits in slot i and the live reading in the slot
 * after the newest sample, so the "now" dot walks right as samples arrive. Once the window is
 * full the oldest sample drops off and the curve scrolls left under a dot pinned at G_X1.
 * Graph index i (0..hist_last()) is sample i, or the live reading when i == hist_n.
 */
static int hist_last(void) { return U.temp_known ? U.hist_n : U.hist_n - 1; }
static float hist_val(int i) { return i < U.hist_n ? U.hist[i] : U.temp_f; }
static float slot_x(int slot) { return G_X0 + (G_X1 - G_X0) * (float)slot / (float)HIST_MAX; }
static float temp_y(float f) { return G_YT + (G_YB - G_YT) * (1.0f - (f - U.g_min) / (U.g_max - U.g_min)); }

static void place_marker(void)
{
    const bool show = U.npts > 0;
    set_hidden(U.halo, !show);
    set_hidden(U.dot, !show);
    if(!show) return;
    int i = U.view == VIEW_HIST ? U.hist_idx : U.npts - 1;
    if(i < 0) i = 0;
    if(i >= U.npts) i = U.npts - 1;
    const brew_pt_t p = U.pts[i];
    const int32_t hs = lv_obj_get_width(U.halo), ds = lv_obj_get_width(U.dot);
    lv_obj_set_pos(U.halo, (int32_t)lroundf(p.x) - hs / 2, (int32_t)lroundf(p.y) - hs / 2);
    lv_obj_set_pos(U.dot, (int32_t)lroundf(p.x) - ds / 2, (int32_t)lroundf(p.y) - ds / 2);
}

static void render_graph(void)
{
    U.graph_dirty = false;
    U.graph_last_ms = lv_tick_get();
    const int n = hist_last() + 1;
    if(n < 1) {
        memset(U.graph_buf, 0, (size_t)DISP * G_H * 4);
        lv_obj_invalidate(U.graph);
        U.npts = 0;
        U.npoly = 0;
        place_marker();
        return;
    }
    const float tg = U.sp_f[U.mode];
    float mn = tg, mx = tg;
    for(int i = 0; i < n; i++) {
        const float v = hist_val(i);
        if(v < mn) mn = v;
        if(v > mx) mx = v;
    }
    if(mx - mn < 1.0f) {   /* at least 1 F tall, centred, so a steady hold sits mid-graph */
        const float mid = (mn + mx) * 0.5f;
        mn = mid - 0.5f;
        mx = mid + 0.5f;
    }
    const float span = mx - mn;
    mn -= span * 0.15f;
    mx += span * 0.15f;
    U.g_min = mn;
    U.g_max = mx;

    for(int i = 0; i < n; i++) {
        U.pts[i].x = slot_x(i);
        U.pts[i].y = temp_y(hist_val(i));
    }
    U.npts = n;
    U.npoly = n < 2 ? 0 : brew_curve_sample(U.pts, n, U.poly, (int)(sizeof(U.poly) / sizeof(U.poly[0])), POLY_STEPS);

    const vstate_t vs = compute_vs();
    const uint32_t lc = s_line[vs];
    brew_graph_fill(U.graph_buf, DISP, G_H, G_Y0, U.poly, U.npoly, lc, 0.38f, 0.02f);

    lv_layer_t layer;
    lv_canvas_init_layer(U.graph, &layer);

    /* setpoint reference: dotted, 22% white */
    const float sy = G_YT + (G_YB - G_YT) * (1.0f - (tg - mn) / (mx - mn)) - (float)G_Y0;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_white();
    d.opa = 56;
    d.width = 1;
    d.dash_width = 2;
    d.dash_gap = 6;
    d.p1.x = 0;
    d.p1.y = sy;
    d.p2.x = DISP;
    d.p2.y = sy;
    lv_draw_line(&layer, &d);

    /* the curve */
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(lc);
    d.opa = LV_OPA_COVER;
    d.width = (int32_t)lroundf(DS(1.8f));
    d.round_start = 1;
    d.round_end = 1;
    for(int i = 0; i < U.npoly - 1; i++) {
        d.p1.x = U.poly[i].x;
        d.p1.y = U.poly[i].y - (float)G_Y0;
        d.p2.x = U.poly[i + 1].x;
        d.p2.y = U.poly[i + 1].y - (float)G_Y0;
        lv_draw_line(&layer, &d);
    }
    lv_canvas_finish_layer(U.graph, &layer);
    lv_obj_invalidate(U.graph);

    set_bg_color(U.halo, lv_color_hex(lc));
    place_marker();
}

static void graph_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    if(U.graph_dirty && lv_tick_elaps(U.graph_last_ms) >= GRAPH_MIN_REDRAW_MS) render_graph();
}

/* ------------------------------------------------------------------ */
/* Refresh                                                            */
/* ------------------------------------------------------------------ */
static void fmt_temp(char * buf, size_t n, float f) { snprintf(buf, n, "%.1f", (double)to_disp(f)); }

static void update_ring(void)
{
    const float target = U.setting ? U.draft_f : U.sp_f[U.mode];
    const float a = frac_t(U.temp_f), b = frac_t(target);
    const float lo = a < b ? a : b, hi = a < b ? b : a;
    lv_arc_set_angles(U.arc, norm_deg(A0 + SWEEP * lo), norm_deg(A0 + SWEEP * hi + 0.6f));
    const lv_color_t ac = lv_color_hex(s_line[compute_vs()]);
    if(!col_eq(lv_obj_get_style_arc_color(U.arc, LV_PART_INDICATOR), ac))
        lv_obj_set_style_arc_color(U.arc, ac, LV_PART_INDICATOR);
    const lv_opa_t ao = U.setting ? LV_OPA_COVER : 179;
    if(lv_obj_get_style_arc_opa(U.arc, LV_PART_INDICATOR) != ao)
        lv_obj_set_style_arc_opa(U.arc, ao, LV_PART_INDICATOR);

    const float ang = (A0 + SWEEP * b) * 3.14159265f / 180.0f;
    const int32_t ks = lv_obj_get_width(U.knob);
    lv_obj_set_pos(U.knob,
                   (int32_t)lroundf(DISP / 2.0f + RING_R * cosf(ang)) - ks / 2,
                   (int32_t)lroundf(DISP / 2.0f + RING_R * sinf(ang)) - ks / 2);
}

static void update_visibility(void)
{
    const bool home = U.view == VIEW_HOME;
    /* Fades re-render everything under the faded object every frame, so they're kept
     * short (BREW_FADE_MS). The ring/ticks cover nearly the whole screen. */
    fade_to(U.v_home, home ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    fade_to(U.v_menu, U.view == VIEW_MENU ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    fade_to(U.v_hist, U.view == VIEW_HIST ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    /* hide fully transparent views so they cost nothing and can't be invalidated */
    set_hidden(U.v_menu, U.view != VIEW_MENU && lv_obj_get_style_opa(U.v_menu, 0) == 0 && !lv_anim_get(U.v_menu, opa_exec));
    set_hidden(U.v_hist, U.view != VIEW_HIST && lv_obj_get_style_opa(U.v_hist, 0) == 0 && !lv_anim_get(U.v_hist, opa_exec));

    fade_to(U.arc, home ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    fade_to(U.knob, home ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    fade_to(U.ticks, home && U.setting ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);

    const lv_opa_t g = U.view == VIEW_MENU ? 64 : (U.setting ? 102 : LV_OPA_COVER);
    fade_to(U.graph, g, BREW_FADE_MS);
    fade_to(U.halo, g, BREW_FADE_MS);
    fade_to(U.dot, g, BREW_FADE_MS);
}

static void update_menu_items(void)
{
    static const char * icons[5] = { BREW_ICON_BEER, BREW_ICON_SNOWFLAKE, BREW_ICON_PAUSE, BREW_ICON_CHART, BREW_ICON_TEMP };
    char units[24];
    snprintf(units, sizeof(units), "Units \xC2\xB7 \xC2\xB0%c", U.celsius ? 'C' : 'F');
    const char * text[5] = { "Ferment", "Cold crash", U.paused ? "Resume" : "Pause", "History", units };

    for(int i = 0; i < 5; i++) {
        const char * ico = icons[i];
        if(i == 2 && U.paused) ico = BREW_ICON_PLAY;
        set_text(U.item_ico[i], ico);
        set_text(U.item_txt[i], text[i]);
        const lv_font_t * f = i == U.menu_idx ? &brew_font_menu_bold : &brew_font_menu;
        set_font(U.item_ico[i], f);
        set_font(U.item_txt[i], f);
        const bool active = !U.paused && ((i == 0 && U.mode == BREW_MODE_FERMENT) ||
                                          (i == 1 && U.mode == BREW_MODE_CRASH));
        set_hidden(U.item_dot[i], !active);
    }
}

static void layout_wheel(void)
{
    /* Watch-style wheel: the centred row is full strength, neighbours fade out.
     * Depth comes from opacity + weight rather than transform_scale: in LVGL 9.3
     * transformed rows that scroll in from outside their parent render clipped. */
    const float pos = (float)U.wheel_pos / 256.0f;
    for(int i = 0; i < 5; i++) {
        const float o = (float)i - pos, ao = fabsf(o);
        float op = 1.0f - 0.5f * ao;
        if(op < 0.0f) op = 0.0f;
        op = op * op * (3.0f - 2.0f * op); /* smoothstep: crisper falloff */
        lv_obj_align(U.item[i], LV_ALIGN_CENTER, 0, (int32_t)lroundf(o * (float)DC(42)));
        lv_obj_set_style_opa(U.item[i], (lv_opa_t)lroundf(op * 255.0f), 0);
        set_hidden(U.item[i], op <= 0.01f);
    }
}

static void wheel_exec(void * v, int32_t x)
{
    LV_UNUSED(v);
    U.wheel_pos = x;
    layout_wheel();
}

static void wheel_animate_to(int idx)
{
    lv_anim_delete(&U.wheel_pos, wheel_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &U.wheel_pos);
    lv_anim_set_exec_cb(&a, wheel_exec);
    lv_anim_set_values(&a, U.wheel_pos, idx * 256);
    lv_anim_set_duration(&a, 320);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void fmt_hist_time(char * buf, size_t n, int idx)
{
    /* sample i covers the interval that ended (hist_n - i) samples ago; hist_n is live */
    const int back_min = (U.hist_n - idx) * U.hist_min_per;
    if(idx >= U.hist_n || back_min <= 0) {
        snprintf(buf, n, "Now");
        return;
    }
    time_t now = time(NULL);
    if(now > 1700000000) { /* wall clock is set (SNTP) */
        time_t t = now - (time_t)back_min * 60;
        struct tm tmv;
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        int h = tmv.tm_hour % 12;
        if(h == 0) h = 12;
        snprintf(buf, n, "%d:%02d %s", h, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
    }
    else {
        snprintf(buf, n, "%dh %02dm ago", back_min / 60, back_min % 60);
    }
}

static void refresh(bool animate)
{
    const vstate_t vs = compute_vs();
    if(!U.vs_valid || vs != U.vs) {
        bg_show(vs, animate && U.vs_valid);
        if(!U.vs_valid || s_line[vs] != s_line[U.vs]) U.graph_dirty = true;  /* line colour changed */
        U.vs = vs;
        U.vs_valid = true;
    }
    if(U.graph_dirty) render_graph();

    char t[24], s[48];
    const lv_color_t white = lv_color_white();
    const lv_color_t cool = lv_color_hex(COOL_TEXT);

    if(U.setting) {
        set_text(U.st_lbl, "Target");
        set_text_color(U.st_lbl, white);
        set_text_opa(U.st_lbl, 179);
        set_hidden(U.st_dot, true);
        nv_set_color(&U.big, white);
        set_text(U.sub_lbl, "Turn to adjust \xC2\xB7 press to set");
        fmt_temp(t, sizeof(t), U.draft_f);
        nv_set(&U.big, t, animate);
    }
    else {
        const int tgt = (int)lroundf(to_disp(U.sp_f[U.mode]));
        switch(vs) {
            case VS_COOL:  snprintf(s, sizeof(s), "Cooling to %d\xC2\xB0", tgt); break;
            case VS_IDLE:  snprintf(s, sizeof(s), "Holding at %d\xC2\xB0", tgt); break;
            case VS_HEAT:  snprintf(s, sizeof(s), "Heating to %d\xC2\xB0", tgt); break;
            case VS_CRASH: snprintf(s, sizeof(s), "Crashing to %d\xC2\xB0", tgt); break;
            default:       snprintf(s, sizeof(s), "Paused"); break;
        }
        set_text(U.st_lbl, s);
        const bool is_cool = vs == VS_COOL;
        set_text_color(U.st_lbl, is_cool ? cool : white);
        set_text_opa(U.st_lbl, is_cool ? LV_OPA_COVER : 224);
        set_bg_color(U.st_dot, is_cool ? cool : white);
        set_hidden(U.st_dot, vs == VS_IDLE || vs == VS_PAUSE);
        nv_set_color(&U.big, is_cool ? cool : white);

        if(U.mode == BREW_MODE_CRASH)
            snprintf(s, sizeof(s), "%s%sCold crash", U.batch, U.batch[0] ? " \xC2\xB7 " : "");
        else
            snprintf(s, sizeof(s), "%s%sDay %d", U.batch, U.batch[0] ? " \xC2\xB7 " : "", U.day);
        set_text(U.sub_lbl, s);
        fmt_temp(t, sizeof(t), U.temp_f);
        nv_set(&U.big, t, animate);
    }

    if(U.view == VIEW_HIST && hist_last() >= 0) {
        fmt_temp(t, sizeof(t), hist_val(U.hist_idx));
        nv_set(&U.hval, t, animate);
        nv_set_color(&U.hval, vs == VS_COOL ? cool : white);
        fmt_hist_time(s, sizeof(s), U.hist_idx);
        set_text(U.htime, s);
    }

    if(U.view == VIEW_MENU) update_menu_items();
    update_ring();
    update_visibility();
    place_marker();
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */
static void commit_setpoint(void)
{
    if(!U.setting) return;
    U.setting = false;
    lv_timer_pause(U.commit_t);
    U.sp_f[U.mode] = U.draft_f;
    U.graph_dirty = true;
    if(U.cbs.on_setpoint) U.cbs.on_setpoint(U.mode, U.draft_f, U.cbs.user);
    refresh(true);
}

static void commit_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    commit_setpoint();
}

static void set_view(view_t v)
{
    U.view = v;
    if(v == VIEW_MENU) {
        lv_anim_delete(&U.wheel_pos, wheel_exec);
        U.menu_idx = 0;
        U.wheel_pos = 0;
        layout_wheel();
    }
    if(v == VIEW_HIST) {
        U.hist_idx = hist_last() > 0 ? hist_last() : 0;
        U.hval.cur[0] = 0; /* no slide-in on entry */
    }
    refresh(false);
}

void brew_ui_encoder(int d)
{
    if(d == 0) return;
    switch(U.view) {
        case VIEW_HOME: {
            if(!U.setting) {
                U.setting = true;
                U.draft_f = U.sp_f[U.mode];
            }
            if(U.celsius) {
                float c = (U.draft_f - 32.0f) * 5.0f / 9.0f;
                c = roundf(c * 2.0f) / 2.0f + 0.5f * (float)d;
                U.draft_f = c * 9.0f / 5.0f + 32.0f;
            }
            else {
                U.draft_f = roundf(U.draft_f * 2.0f) / 2.0f + 0.5f * (float)d;
            }
            if(U.draft_f < T_MIN) U.draft_f = T_MIN;
            if(U.draft_f > T_MAX) U.draft_f = T_MAX;
            lv_timer_reset(U.commit_t);
            lv_timer_resume(U.commit_t);
            refresh(true);
            break;
        }
        case VIEW_MENU: {
            int i = U.menu_idx + d;
            if(i < 0) i = 0;
            if(i > 4) i = 4;
            if(i != U.menu_idx) {
                U.menu_idx = i;
                update_menu_items();
                wheel_animate_to(i);
            }
            break;
        }
        case VIEW_HIST: {
            int i = U.hist_idx + d;
            if(i > hist_last()) i = hist_last();
            if(i < 0) i = 0;
            if(i != U.hist_idx) {
                U.hist_idx = i;
                refresh(true);
            }
            break;
        }
    }
}

void brew_ui_press(void)
{
    switch(U.view) {
        case VIEW_HOME:
            if(U.setting) commit_setpoint();
            else set_view(VIEW_MENU);
            break;
        case VIEW_HIST:
            set_view(VIEW_HOME);
            break;
        case VIEW_MENU:
            switch(U.menu_idx) {
                case 0:
                case 1:
                    U.mode = U.menu_idx == 0 ? BREW_MODE_FERMENT : BREW_MODE_CRASH;
                    U.paused = false;
                    U.graph_dirty = true;
                    if(U.cbs.on_mode) U.cbs.on_mode(U.mode, U.paused, U.cbs.user);
                    set_view(VIEW_HOME);
                    break;
                case 2:
                    U.paused = !U.paused;
                    if(U.cbs.on_mode) U.cbs.on_mode(U.mode, U.paused, U.cbs.user);
                    set_view(VIEW_HOME);
                    break;
                case 3:
                    set_view(VIEW_HIST);
                    break;
                case 4:
                    U.celsius = !U.celsius;
                    if(U.cbs.on_units) U.cbs.on_units(U.celsius, U.cbs.user);
                    refresh(false);
                    break;
            }
            break;
    }
}

static void tap_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    brew_ui_press();
}

/* ------------------------------------------------------------------ */
/* Construction                                                       */
/* ------------------------------------------------------------------ */
static lv_obj_t * make_canvas(lv_obj_t * parent, void * buf, int32_t w, int32_t h, lv_color_format_t cf)
{
    lv_obj_t * c = lv_canvas_create(parent);
    lv_canvas_set_buffer(c, buf, w, h, cf);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static lv_obj_t * make_view(lv_obj_t * parent, int32_t pad_top, int32_t gap)
{
    lv_obj_t * v = plain_obj(parent);
    lv_obj_set_size(v, DISP, DISP);
    lv_obj_set_flex_flow(v, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(v, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(v, pad_top, 0);
    lv_obj_set_style_pad_row(v, gap, 0);
    return v;
}

static void breathe_start(lv_obj_t * o)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, 255, 64);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_reverse_duration(&a, 1200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void brew_ui_create(lv_obj_t * screen, const brew_ui_cbs_t * cbs)
{
    memset(&U, 0, sizeof(U));
    if(cbs) U.cbs = *cbs;
    U.temp_f = 66.0f;
    U.sp_f[0] = 66.0f;
    U.sp_f[1] = 34.0f;
    U.hist_min_per = 1;

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    U.root = plain_obj(screen);
    lv_obj_set_size(U.root, DISP, DISP);
    lv_obj_center(U.root);
    lv_obj_add_flag(U.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.root, tap_cb, LV_EVENT_CLICKED, NULL);

    /* backgrounds */
    U.bg_gid = -1;
    for(int g = 0; g < 4; g++) bg_get(g);   /* pre-render now so state changes never stall */
    for(int i = 0; i < 2; i++) {
        U.bg[i] = make_canvas(U.root, bg_get(0), DISP, DISP, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(U.bg[i], 0, 0);
    }
    lv_obj_set_style_opa(U.bg[1], LV_OPA_TRANSP, 0);

    /* graph */
    U.graph_buf = BREW_UI_ALLOC((size_t)DISP * G_H * 4);
    LV_ASSERT_MALLOC(U.graph_buf);
    memset(U.graph_buf, 0, (size_t)DISP * G_H * 4);
    U.graph = make_canvas(U.root, U.graph_buf, DISP, G_H, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(U.graph, 0, G_Y0);

    U.halo = plain_obj(U.root);
    lv_obj_set_size(U.halo, (int32_t)lroundf(DS(12)), (int32_t)lroundf(DS(12)));
    lv_obj_set_style_radius(U.halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.halo, 64, 0);
    U.dot = plain_obj(U.root);
    lv_obj_set_size(U.dot, (int32_t)lroundf(DS(6)), (int32_t)lroundf(DS(6)));
    lv_obj_set_style_radius(U.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(U.dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(U.dot, LV_OPA_COVER, 0);

    /* ticks (only while adjusting) */
    const int32_t tick_d = (int32_t)lroundf(DS(107) * 2.0f);
    U.ticks = lv_scale_create(U.root);
    lv_obj_remove_style_all(U.ticks);
    lv_obj_set_size(U.ticks, tick_d, tick_d);
    lv_obj_center(U.ticks);
    lv_scale_set_mode(U.ticks, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(U.ticks, (int32_t)T_MIN, (int32_t)T_MAX);
    lv_scale_set_total_tick_count(U.ticks, (uint32_t)(T_MAX - T_MIN) + 1);
    lv_scale_set_major_tick_every(U.ticks, 5);
    lv_scale_set_label_show(U.ticks, false);
    lv_scale_set_angle_range(U.ticks, (uint32_t)SWEEP);
    lv_scale_set_rotation(U.ticks, (int32_t)A0);
    lv_obj_set_style_arc_width(U.ticks, 0, LV_PART_MAIN);
    lv_obj_set_style_line_color(U.ticks, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_line_opa(U.ticks, 64, LV_PART_ITEMS);
    lv_obj_set_style_line_width(U.ticks, 1, LV_PART_ITEMS);
    lv_obj_set_style_length(U.ticks, (int32_t)lroundf(DS(3)), LV_PART_ITEMS);
    lv_obj_set_style_line_color(U.ticks, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_line_opa(U.ticks, 153, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(U.ticks, 2, LV_PART_INDICATOR);
    lv_obj_set_style_length(U.ticks, (int32_t)lroundf(DS(6)), LV_PART_INDICATOR);
    lv_obj_set_style_line_rounded(U.ticks, true, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(U.ticks, true, LV_PART_INDICATOR);
    lv_obj_set_style_opa(U.ticks, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(U.ticks, LV_OBJ_FLAG_CLICKABLE);

    /* setpoint ring */
    const int32_t ring_w = (int32_t)lroundf(DS(3));
    const int32_t ring_d = (int32_t)lroundf(RING_R * 2.0f) + ring_w;
    U.arc = lv_arc_create(U.root);
    lv_obj_remove_style_all(U.arc);
    lv_obj_set_size(U.arc, ring_d, ring_d);
    lv_obj_center(U.arc);
    lv_arc_set_rotation(U.arc, 0);
    lv_arc_set_bg_angles(U.arc, (lv_value_precise_t)A0, (lv_value_precise_t)norm_deg(A0 + SWEEP));
    lv_obj_set_style_arc_width(U.arc, ring_w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(U.arc, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(U.arc, 26, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(U.arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(U.arc, ring_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(U.arc, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(U.arc, LV_OBJ_FLAG_CLICKABLE);

    U.knob = plain_obj(U.root);
    lv_obj_set_size(U.knob, (int32_t)lroundf(DS(10)), (int32_t)lroundf(DS(10)));
    lv_obj_set_style_radius(U.knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(U.knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(U.knob, LV_OPA_COVER, 0);

    /* ---- home view ---- */
    U.v_home = make_view(U.root, DC(62), DC(10));
    lv_obj_t * st_row = plain_obj(U.v_home);
    lv_obj_set_size(st_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st_row, DC(7), 0);
    U.st_dot = plain_obj(st_row);
    lv_obj_set_size(U.st_dot, DC(6), DC(6));
    lv_obj_set_style_radius(U.st_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.st_dot, LV_OPA_COVER, 0);
    breathe_start(U.st_dot);
    U.st_lbl = lv_label_create(st_row);
    lv_obj_set_style_text_font(U.st_lbl, &brew_font_status, 0);

    nv_create(&U.big, U.v_home, &brew_font_num_big, &brew_font_deg_big, DC(17));

    U.sub_lbl = lv_label_create(U.v_home);
    lv_obj_set_style_text_font(U.sub_lbl, &brew_font_small, 0);
    lv_obj_set_style_text_color(U.sub_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_opa(U.sub_lbl, 140, 0);

    /* ---- menu view ---- */
    U.v_menu = plain_obj(U.root);
    lv_obj_set_size(U.v_menu, DISP, DISP);
    lv_obj_set_style_opa(U.v_menu, LV_OPA_TRANSP, 0);
    for(int i = 0; i < 5; i++) {
        lv_obj_t * it = plain_obj(U.v_menu);
        lv_obj_set_size(it, LV_SIZE_CONTENT, DC(44));
        lv_obj_set_flex_flow(it, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(it, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(it, DC(10), 0);
        U.item_ico[i] = lv_label_create(it);
        lv_obj_set_style_text_color(U.item_ico[i], lv_color_white(), 0);
        lv_obj_set_style_text_opa(U.item_ico[i], 230, 0);
        U.item_txt[i] = lv_label_create(it);
        lv_obj_set_style_text_color(U.item_txt[i], lv_color_white(), 0);
        U.item_dot[i] = plain_obj(it);
        lv_obj_set_size(U.item_dot[i], DC(5), DC(5));
        lv_obj_set_style_radius(U.item_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(U.item_dot[i], lv_color_hex(COOL_TEXT), 0);
        lv_obj_set_style_bg_opa(U.item_dot[i], LV_OPA_COVER, 0);
        U.item[i] = it;
    }

    /* ---- history view ---- */
    U.v_hist = make_view(U.root, DC(58), DC(7));
    lv_obj_set_style_opa(U.v_hist, LV_OPA_TRANSP, 0);
    U.htime = lv_label_create(U.v_hist);
    lv_obj_set_style_text_font(U.htime, &brew_font_small, 0);
    lv_obj_set_style_text_color(U.htime, lv_color_white(), 0);
    lv_obj_set_style_text_opa(U.htime, 140, 0);
    nv_create(&U.hval, U.v_hist, &brew_font_num_hist, &brew_font_deg_hist, DC(12));
    lv_obj_t * hs = lv_label_create(U.v_hist);
    lv_obj_set_style_text_font(hs, &brew_font_small, 0);
    lv_obj_set_style_text_color(hs, lv_color_white(), 0);
    lv_obj_set_style_text_opa(hs, 140, 0);
    lv_label_set_text(hs, "Turn to scrub \xC2\xB7 last 24 hours");

    U.commit_t = lv_timer_create(commit_timer_cb, COMMIT_MS, NULL);
    lv_timer_pause(U.commit_t);
    U.graph_t = lv_timer_create(graph_timer_cb, 2000, NULL);

    U.view = VIEW_HOME;
    update_menu_items();
    layout_wheel();
    refresh(false);
}

/* ------------------------------------------------------------------ */
/* Controller -> UI                                                   */
/* ------------------------------------------------------------------ */
void brew_ui_set_temp(float f)
{
    const bool first = !U.temp_known;
    U.temp_f = f;
    U.temp_known = true;
    if(first) {
        U.graph_dirty = true;   /* the live point appears: draw it now */
        refresh(true);
        return;
    }
    /* The live point moved: redraw only if that shows (>= 1 px or off scale), and leave it to
     * graph_t, which re-renders at most every GRAPH_MIN_REDRAW_MS. */
    if(U.npts == 0 || fabsf(temp_y(f) - U.pts[U.npts - 1].y) >= 1.0f || f < U.g_min || f > U.g_max)
        U.graph_dirty = true;
    const bool g = U.graph_dirty;
    U.graph_dirty = false;    /* don't re-render synchronously here */
    refresh(true);
    U.graph_dirty = g;
}

void brew_ui_set_action(brew_action_t a)
{
    U.action = a;
    refresh(true);
}

void brew_ui_set_setpoints(float ferment_f, float crash_f)
{
    U.sp_f[0] = ferment_f;
    U.sp_f[1] = crash_f;
    U.graph_dirty = true;
    refresh(false);
}

void brew_ui_set_mode(brew_mode_t m, bool paused)
{
    U.mode = m;
    U.paused = paused;
    U.graph_dirty = true;
    refresh(true);
}

void brew_ui_set_units(bool celsius)
{
    U.celsius = celsius;
    refresh(false);
}

void brew_ui_set_batch(const char * name, int day)
{
    snprintf(U.batch, sizeof(U.batch), "%s", name ? name : "");
    U.day = day;
    refresh(false);
}

void brew_ui_set_history(const float * t, int n, int minutes_per_sample)
{
    if(n > HIST_MAX) {
        t += n - HIST_MAX;
        n = HIST_MAX;
    }
    if(n < 0) n = 0;
    if(n > 0) memcpy(U.hist, t, (size_t)n * sizeof(float));
    U.hist_n = n;
    if(minutes_per_sample > 0) U.hist_min_per = minutes_per_sample;
    if(U.view == VIEW_HIST) U.hist_idx = hist_last() > 0 ? hist_last() : 0;
    U.graph_dirty = true;
    refresh(false);
}

void brew_ui_push_history(float f)
{
    const bool was_live = U.hist_idx >= U.hist_n;   /* scrubbing: keep the same moment selected */
    if(U.hist_n == HIST_MAX) {
        memmove(U.hist, U.hist + 1, (HIST_MAX - 1) * sizeof(float));
        U.hist_n--;
        if(!was_live && U.hist_idx > 0) U.hist_idx--;
    }
    U.hist[U.hist_n++] = f;
    if(was_live) U.hist_idx = hist_last() > 0 ? hist_last() : 0;
    U.graph_dirty = true;   /* drawn by graph_t within GRAPH_MIN_REDRAW_MS */
}

