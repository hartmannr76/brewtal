/**
 * layout_kit.c - drawing pieces shared by the layouts. See layout_kit.h.
 */
#include "layout_kit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "Smoked glass": a near-black face lit from below by the state colour (the web harness's
 * palette: --s, --s-deep). Light at (50%, 87.5%), reaching 70% of the width. */
#define GLASS(s, deep) { 0x0b0e11, (s), (deep), 0.5f, 0.875f, 0.708f, 0.6f }
static const brew_glass_t s_glass[BREW_VS_N] = {
    GLASS(0x3070b0, 0x20456b),   /* cool  */
    GLASS(0x3070b0, 0x20456b),   /* idle  */
    GLASS(0xff8f3a, 0xc4561a),   /* heat  */
    GLASS(0x8c5ff2, 0x4b2e98),   /* crash */
    GLASS(0x5f6871, 0x383f46),   /* pause */
    GLASS(0x2f8f5e, 0x1d5038),   /* ok (WiFi connected) */
};
const uint32_t kit_line[BREW_VS_N] = { 0x7cd4ff, 0x7cd4ff, 0xffcf8a, 0xd8bcff, 0xc3cad1, 0x8fe3b0 };
const uint32_t kit_base[BREW_VS_N] = { 0x3070b0, 0x3070b0, 0xff8f3a, 0x8c5ff2, 0x5f6871, 0x2f8f5e };

/* ------------------------------------------------------------------ setters */

bool kit_col_eq(lv_color_t a, lv_color_t b) { return a.red == b.red && a.green == b.green && a.blue == b.blue; }

void kit_arc_set_angles(lv_obj_t * arc, lv_value_precise_t start, lv_value_precise_t end)
{
    if(start == lv_arc_get_angle_start(arc) && end == lv_arc_get_angle_end(arc)) return;
    /* which way the end moves, the short way round */
    lv_value_precise_t d = end - lv_arc_get_angle_end(arc);
    while(d > 180) d -= 360;
    while(d <= -180) d += 360;
    if(d < 0) {   /* backwards: the start leads */
        lv_arc_set_start_angle(arc, start);
        lv_arc_set_end_angle(arc, end);
    }
    else {        /* forwards: the end leads (what lv_arc_set_angles does) */
        lv_arc_set_end_angle(arc, end);
        lv_arc_set_start_angle(arc, start);
    }
}

void kit_set_text(lv_obj_t * l, const char * t)
{
    const char * c = lv_label_get_text(l);
    if(c && strcmp(c, t) == 0) return;
    lv_label_set_text(l, t);
}
void kit_set_text_color(lv_obj_t * o, lv_color_t c)
{
    if(!kit_col_eq(lv_obj_get_style_text_color(o, 0), c)) lv_obj_set_style_text_color(o, c, 0);
}
void kit_set_text_opa(lv_obj_t * o, lv_opa_t v)
{
    if(lv_obj_get_style_text_opa(o, 0) != v) lv_obj_set_style_text_opa(o, v, 0);
}
void kit_set_bg_color(lv_obj_t * o, lv_color_t c)
{
    if(!kit_col_eq(lv_obj_get_style_bg_color(o, 0), c)) lv_obj_set_style_bg_color(o, c, 0);
}
void kit_set_font(lv_obj_t * o, const lv_font_t * f)
{
    if(lv_obj_get_style_text_font(o, 0) != f) lv_obj_set_style_text_font(o, f, 0);
}
void kit_set_hidden(lv_obj_t * o, bool h)
{
    if(lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == h) return;
    if(h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t * kit_plain(lv_obj_t * parent)
{
    lv_obj_t * o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t * kit_canvas(lv_obj_t * parent, void * buf, int32_t w, int32_t h, lv_color_format_t cf)
{
    lv_obj_t * c = lv_canvas_create(parent);
    lv_canvas_set_buffer(c, buf, w, h, cf);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

void kit_opa_exec(void * o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

void kit_fade_to(lv_obj_t * o, lv_opa_t to, uint32_t ms)
{
    lv_anim_delete(o, kit_opa_exec);
    lv_opa_t from = lv_obj_get_style_opa(o, 0);
    if(to > 0 && lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    if(from == to) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, kit_opa_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void kit_breathe(lv_obj_t * o)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, kit_opa_exec);
    lv_anim_set_values(&a, 255, 64);
    lv_anim_set_duration(&a, 1200);
    lv_anim_set_reverse_duration(&a, 1200);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/* ------------------------------------------------------------------ number view */

static void ty_exec(void * o, int32_t v) { lv_obj_set_style_translate_y((lv_obj_t *)o, v, 0); }
static void tx_opa_exec(void * o, int32_t v) { lv_obj_set_style_text_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static int32_t glyph_w(const lv_font_t * f, uint32_t c)
{
    return (int32_t)lv_font_get_glyph_width(f, c, 0);
}

void kit_nv_create(kit_numview_t * nv, lv_obj_t * parent, const lv_font_t * font, const lv_font_t * deg_font,
                   int32_t deg_dy, int32_t deg_pad)
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

    for(int i = 0; i < KIT_NV_MAX; i++) {
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
    lv_obj_set_style_pad_left(nv->deg, deg_pad, 0);
    lv_label_set_text(nv->deg, "\xC2\xB0");
}

void kit_nv_set_unit(kit_numview_t * nv, const char * unit) { kit_set_text(nv->deg, unit); }

void kit_nv_set_color(kit_numview_t * nv, lv_color_t c)
{
    if(nv->color_set && kit_col_eq(nv->color, c)) return;
    nv->color = c;
    nv->color_set = true;
    for(int i = 0; i < KIT_NV_MAX; i++) lv_obj_set_style_text_color(nv->lbl[i], c, 0);
    lv_obj_set_style_text_color(nv->deg, c, 0);
}

static void nv_anim_in(kit_numview_t * nv, lv_obj_t * l, bool up)
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

void kit_nv_set(kit_numview_t * nv, const char * str, bool animate)
{
    if(strcmp(nv->cur, str) == 0) return;
    const bool up = atof(str) >= atof(nv->cur);
    const int L = (int)strlen(str), Lo = (int)strlen(nv->cur);
    const bool had_old = Lo > 0;

    for(int k = 0; k < KIT_NV_MAX; k++) {
        const int si = L - KIT_NV_MAX + k;   /* right-aligned slot mapping */
        const int oi = Lo - KIT_NV_MAX + k;
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
    strncpy(nv->cur, str, KIT_NV_MAX);
    nv->cur[KIT_NV_MAX] = 0;
}

/* ------------------------------------------------------------------ background */

#define KIT_NGRAD 5
static uint16_t * s_bg_cache[KIT_NGRAD];   /* one pre-rendered background per distinct gradient */

static int grad_id(brew_vstate_t vs)
{
    switch(vs) {
        case BREW_VS_HEAT:  return 1;
        case BREW_VS_CRASH: return 2;
        case BREW_VS_PAUSE: return 3;
        case BREW_VS_OK:    return 4;
        default:            return 0;   /* cooling and holding share the blue gradient */
    }
}
static const brew_vstate_t s_gid_vs[KIT_NGRAD] = { BREW_VS_COOL, BREW_VS_HEAT, BREW_VS_CRASH, BREW_VS_PAUSE, BREW_VS_OK };

/* Rendering a background takes a while on the MCU, so startup renders only the one it shows
 * first; the rest are filled in a few rows at a time while the UI is idle. A state change
 * that needs one before then finishes it on the spot. */
static int s_bg_rows[KIT_NGRAD];        /* rows rendered so far (KIT_DISP = ready) */
static lv_timer_t * s_bg_timer;
#define KIT_BG_IDLE_ROWS 24             /* per idle step */
#define KIT_BG_IDLE_MS   30

static void bg_render(int gid, int rows)
{
    if(!s_bg_cache[gid]) {
        s_bg_cache[gid] = BREW_UI_ALLOC((size_t)KIT_DISP * KIT_DISP * 2);
        LV_ASSERT_MALLOC(s_bg_cache[gid]);
        s_bg_rows[gid] = 0;
    }
    const int y0 = s_bg_rows[gid], y1 = y0 + rows < KIT_DISP ? y0 + rows : KIT_DISP;
    if(y1 <= y0) return;
    brew_bg_render_glass_rows(s_bg_cache[gid], KIT_DISP, KIT_DISP, &s_glass[s_gid_vs[gid]], y0, y1);
    s_bg_rows[gid] = y1;
}

static uint16_t * bg_get(int gid)
{
    bg_render(gid, KIT_DISP);
    return s_bg_cache[gid];
}

static void bg_idle_cb(lv_timer_t * t)
{
    for(int g = 0; g < KIT_NGRAD; g++) {
        if(s_bg_cache[g] && s_bg_rows[g] >= KIT_DISP) continue;
        bg_render(g, KIT_BG_IDLE_ROWS);
        return;
    }
    lv_timer_delete(t);   /* all ready */
    s_bg_timer = NULL;
}

static void bg_fade_done(kit_bg_t * b)
{
    const int nf = 1 - b->front;
    lv_obj_set_style_opa(b->bg[b->front], LV_OPA_TRANSP, 0);
    lv_obj_move_to_index(b->bg[nf], 0);
    lv_obj_move_to_index(b->bg[b->front], 1);
    b->front = nf;
}
static void bg_fade_done_cb(lv_anim_t * a) { bg_fade_done((kit_bg_t *)lv_anim_get_user_data(a)); }

void kit_bg_create(kit_bg_t * b, lv_obj_t * parent)
{
    b->front = 0;
    b->gid = -1;
    bg_get(0);   /* what the canvases show until the first state; the others render when idle */
    if(!s_bg_timer) s_bg_timer = lv_timer_create(bg_idle_cb, KIT_BG_IDLE_MS, NULL);
    for(int i = 0; i < 2; i++) {
        b->bg[i] = kit_canvas(parent, bg_get(0), KIT_DISP, KIT_DISP, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(b->bg[i], 0, 0);
    }
    lv_obj_set_style_opa(b->bg[1], LV_OPA_TRANSP, 0);
}

void kit_bg_show(kit_bg_t * b, brew_vstate_t vs, bool animate)
{
    const int gid = grad_id(vs);
    if(gid == b->gid) return;            /* same gradient: nothing to redraw */
    const int back = 1 - b->front;
    /* finish an in-flight fade first */
    if(lv_anim_get(b->bg[back], kit_opa_exec)) {
        lv_anim_delete(b->bg[back], kit_opa_exec);
        lv_obj_set_style_opa(b->bg[back], LV_OPA_COVER, 0);
        bg_fade_done(b);
        kit_bg_show(b, vs, animate);
        return;
    }
    b->gid = gid;
    if(!animate) {
        lv_canvas_set_buffer(b->bg[b->front], bg_get(gid), KIT_DISP, KIT_DISP, LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(b->bg[b->front]);
        return;
    }
    lv_canvas_set_buffer(b->bg[back], bg_get(gid), KIT_DISP, KIT_DISP, LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(b->bg[back]);
    lv_obj_set_style_opa(b->bg[back], LV_OPA_TRANSP, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, b->bg[back]);
    lv_anim_set_exec_cb(&a, kit_opa_exec);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, BREW_BG_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_user_data(&a, b);
    lv_anim_set_completed_cb(&a, bg_fade_done_cb);
    lv_anim_start(&a);
}

/* ------------------------------------------------------------------ graph */

/* Point i of n, stretched across x0..x1: the newest (the live reading) always sits at x1, and
 * however few there are, they span the whole width. */
static float point_x(const kit_graph_t * g, int i, int n)
{
    if(n < 2) return g->x1;
    return g->x0 + (g->x1 - g->x0) * (float)i / (float)(n - 1);
}

float kit_graph_index_x(const kit_graph_t * g, int i) { return point_x(g, i, g->nsrc); }

float kit_graph_temp_y(const kit_graph_t * g, float f)
{
    if(f < g->g_min) f = g->g_min;   /* off the fixed scale: ride the edge */
    if(f > g->g_max) f = g->g_max;
    return g->yt + (g->yb - g->yt) * (1.0f - (f - g->g_min) / (g->g_max - g->g_min));
}

void kit_graph_create(kit_graph_t * g, lv_obj_t * parent)
{
    g->buf = BREW_UI_ALLOC((size_t)g->cw * g->ch * 4);
    LV_ASSERT_MALLOC(g->buf);
    memset(g->buf, 0, (size_t)g->cw * g->ch * 4);
    g->canvas = kit_canvas(parent, g->buf, g->cw, g->ch, LV_COLOR_FORMAT_ARGB8888);
    g->rows0 = 0;
    g->rows1 = g->ch - 1;
    lv_obj_set_pos(g->canvas, g->cx, g->cy);

    g->halo = kit_plain(parent);
    lv_obj_set_size(g->halo, g->halo_d, g->halo_d);
    lv_obj_set_style_radius(g->halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g->halo, 64, 0);
    g->dot = kit_plain(parent);
    lv_obj_set_size(g->dot, g->dot_d, g->dot_d);
    lv_obj_set_style_radius(g->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g->dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(g->dot, LV_OPA_COVER, 0);
}

void kit_graph_free(kit_graph_t * g)
{
    if(g->buf) BREW_UI_FREE(g->buf);
    g->buf = NULL;
}

int kit_graph_marker_index(const kit_graph_t * g, const brew_model_t * m)
{
    if(g->npts <= 0) return -1;
    int i = g->nsrc - 1;   /* the live reading */
    if(i < 0) i = 0;
    if(i >= g->nsrc) i = g->nsrc - 1;
    return i;
}

void kit_graph_place_marker(kit_graph_t * g, const brew_model_t * m)
{
    const int i = kit_graph_marker_index(g, m);
    kit_set_hidden(g->halo, i < 0);
    kit_set_hidden(g->dot, i < 0);
    if(i < 0) return;
    /* the sample itself, not the averaged point it was drawn in: scrubbing reads exact values */
    brew_pt_t p;
    p.x = kit_graph_index_x(g, i);
    p.y = i == g->nsrc - 1 ? g->pts[g->npts - 1].y : kit_graph_temp_y(g, brew_model_hist_val(m, i));
    const int32_t hs = g->halo_d, ds = g->dot_d;   /* not lv_obj_get_width(): 0 until the first layout pass */
    lv_obj_set_pos(g->halo, (int32_t)lroundf(p.x) - hs / 2, (int32_t)lroundf(p.y) - hs / 2);
    lv_obj_set_pos(g->dot, (int32_t)lroundf(p.x) - ds / 2, (int32_t)lroundf(p.y) - ds / 2);
}

bool kit_graph_live_moved(const kit_graph_t * g, float f)
{
    return g->npts == 0 || fabsf(kit_graph_temp_y(g, f) - g->pts[g->npts - 1].y) >= 1.0f;
}

void kit_graph_render(kit_graph_t * g, const brew_model_t * m)
{
    g->dirty = false;
    g->last_ms = lv_tick_get();
    const int n = brew_model_hist_last(m) + 1;
    if(n < 1) {
        memset(g->buf, 0, (size_t)g->cw * g->ch * 4);
        lv_obj_invalidate(g->canvas);
        g->rows0 = 0;
        g->rows1 = g->ch - 1;
        g->npts = 0;
        g->ndrawn = 0;
        g->nsrc = 0;
        g->npoly = 0;
        kit_graph_place_marker(g, m);
        return;
    }
    const float tg = m->sp_f[m->mode];
    const float mn = BREW_T_MIN, mx = BREW_T_MAX;   /* fixed scale: never rescaled */
    g->g_min = mn;
    g->g_max = mx;

    g->nsrc = n;
    if(n <= KIT_GRAPH_PTS) {
        for(int i = 0; i < n; i++) {
            g->pts[i].x = point_x(g, i, n);
            g->pts[i].y = kit_graph_temp_y(g, brew_model_hist_val(m, i));
        }
        g->npts = n;
    }
    else {
        /* Average the samples into KIT_GRAPH_PTS - 1 buckets; the live reading stays the last
         * point exactly, so the "now" dot sits on the end of the curve. */
        const int k = KIT_GRAPH_PTS - 1, ns = n - 1;
        for(int j = 0; j < k; j++) {
            const int a = (int)((long)j * ns / k), b = (int)((long)(j + 1) * ns / k);
            float sum = 0.0f;
            for(int i = a; i < b; i++) sum += brew_model_hist_val(m, i);
            g->pts[j].x = point_x(g, j, KIT_GRAPH_PTS);
            g->pts[j].y = kit_graph_temp_y(g, sum / (float)(b - a));
        }
        g->pts[k].x = g->x1;
        g->pts[k].y = kit_graph_temp_y(g, brew_model_hist_val(m, n - 1));
        g->npts = KIT_GRAPH_PTS;
    }
    {
        /* smooth all but the live point; the window shrinks near the ends so the curve still
         * starts at the oldest point and meets the live one */
        static float sm[KIT_GRAPH_PTS];
        const int last = g->npts - 1;
        for(int i = 0; i < last; i++) {
            int r = KIT_GRAPH_SMOOTH;
            if(i < r) r = i;
            if(last - i < r) r = last - i;
            float sum = 0.0f, wsum = 0.0f;
            for(int j = -r; j <= r; j++) {
                const float w = (float)(r + 1 - (j < 0 ? -j : j));
                sum += g->pts[i + j].y * w;
                wsum += w;
            }
            sm[i] = sum / wsum;
        }
        for(int i = 0; i < last; i++) g->pts[i].y = sm[i];
    }
    int steps = g->npts > 1 ? (KIT_POLY_MAX - 1) / (g->npts - 1) : 1;
    if(steps > KIT_POLY_STEPS) steps = KIT_POLY_STEPS;
    if(steps < 1) steps = 1;
    g->npoly = g->npts < 2 ? 0 : brew_curve_sample(g->pts, g->npts, g->poly, KIT_POLY_MAX, steps);

    const uint32_t lc = kit_line[brew_model_vstate(m)];
    const bool mist = m->graph_style == BREW_GRAPH_MIST;
    const float sy = g->yt + (g->yb - g->yt) * (1.0f - (tg - mn) / (mx - mn));   /* target, screen y */

    /* A new sample in a long window moves the drawn points by a fraction of a pixel: if nothing
     * moved by a visible amount, leave the canvas alone (no redraw, no flush). */
    bool same = g->ndrawn == g->npts && g->drawn_lc == lc && g->drawn_style == (int)m->graph_style &&
                fabsf(g->drawn_sy - sy) < 0.25f;
    for(int i = 0; same && i < g->npts; i++)
        same = fabsf(g->drawn[i].x - g->pts[i].x) < 0.25f && fabsf(g->drawn[i].y - g->pts[i].y) < 0.25f;
    if(same) {
        kit_graph_place_marker(g, m);
        return;
    }
    memcpy(g->drawn, g->pts, sizeof(brew_pt_t) * (size_t)g->npts);
    g->ndrawn = g->npts;
    g->drawn_lc = lc;
    g->drawn_style = (int)m->graph_style;
    g->drawn_sy = sy;
    /* the fill works in parent rows; shift x into the canvas (a no-op for full-width canvases) */
    if(g->cx != 0)
        for(int i = 0; i < g->npoly; i++) g->poly[i].x -= (float)g->cx;

    /* Everything is drawn straight into the canvas pixels (no LVGL draw tasks), and only the
     * rows the curve uses are cleared, drawn and invalidated: this runs every 200 ms. */
    const float glow_w = mist ? g->line_w * 3.6f : g->line_w;
    float ylo = sy, yhi = sy;
    for(int i = 0; i < g->npoly; i++) {
        if(g->poly[i].y < ylo) ylo = g->poly[i].y;
        if(g->poly[i].y > yhi) yhi = g->poly[i].y;
    }
    int32_t r0 = (int32_t)floorf(ylo - glow_w * 0.5f - 2.0f) - g->cy;
    int32_t r1 = mist ? g->ch - 1 : (int32_t)ceilf(yhi + glow_w * 0.5f + 2.0f) - g->cy;   /* the fill runs to the bottom */
    if(r0 < 0) r0 = 0;
    if(r1 > g->ch - 1) r1 = g->ch - 1;
    if(r1 < r0) r1 = r0;

    if(mist) brew_graph_fill(g->buf, g->cw, g->ch, g->cy, g->poly, g->npoly, lc, 0.38f, 0.0f);
    else if(g->rows1 >= g->rows0)
        memset(g->buf + (size_t)g->rows0 * g->cw * 4, 0, (size_t)(g->rows1 - g->rows0 + 1) * g->cw * 4);

    /* the curve; Mist puts a soft glow under it (two wider, faint passes) */
    if(mist) {
        brew_graph_stroke(g->buf, g->cw, g->ch, g->cy, g->poly, g->npoly, lc, g->line_w * 3.6f, 18.0f / 255.0f);
        brew_graph_stroke(g->buf, g->cw, g->ch, g->cy, g->poly, g->npoly, lc, g->line_w * 2.2f, 36.0f / 255.0f);
    }
    brew_graph_stroke(g->buf, g->cw, g->ch, g->cy, g->poly, g->npoly, lc, g->line_w, 1.0f);

    /* fade in from the left, cut to the round glass (only the rows drawn in) */
    const float fx0 = g->x0 - (float)g->cx;
    const float fx1 = fx0 + (g->x1 - g->x0) * (mist ? KIT_GRAPH_FADE_MIST : KIT_GRAPH_FADE_HAIRLINE);
    brew_graph_mask(g->buf + (size_t)r0 * g->cw * 4, g->cw, r1 - r0 + 1, g->cy + r0, fx0, fx1,
                    g->clip_cx - (float)g->cx, g->clip_cy, g->clip_r);

    /* setpoint reference on top, unfaded: dotted, 22% white, across the plot */
    brew_graph_dash_h(g->buf, g->cw, g->ch, g->cy, fx0, g->x1 - (float)g->cx, sy, 0xFFFFFF, 56.0f / 255.0f, 2, 6);

    /* redraw what changed: the rows drawn last time and this time */
    lv_area_t a;
    lv_obj_get_coords(g->canvas, &a);
    const int32_t top = a.y1;
    a.y1 = top + (r0 < g->rows0 ? r0 : g->rows0);
    a.y2 = top + (r1 > g->rows1 ? r1 : g->rows1);
    lv_obj_invalidate_area(g->canvas, &a);
    g->rows0 = r0;
    g->rows1 = r1;

    kit_set_bg_color(g->halo, lv_color_hex(lc));
    kit_graph_place_marker(g, m);
}

/* ------------------------------------------------------------------ WiFi view */

#define KS(x) ((int32_t)lroundf((float)(x) * (float)KIT_DISP / 480.0f))

static lv_obj_t * wifi_label(lv_obj_t * parent, const lv_font_t * f, uint32_t col)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, "");
    return l;
}

void kit_wifi_create(kit_wifi_t * w, lv_obj_t * parent, int32_t x, int32_t y, int32_t wd, int32_t h, int32_t qr_px)
{
    memset(w, 0, sizeof(*w));
    w->root = kit_plain(parent);
    lv_obj_set_pos(w->root, x, y);
    lv_obj_set_size(w->root, wd, h);
    lv_obj_set_flex_flow(w->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w->root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(w->root, KS(10), 0);
    lv_obj_set_style_opa(w->root, LV_OPA_TRANSP, 0);

    /* pill: "● WIFI SETUP" */
    w->pill = kit_plain(w->root);
    lv_obj_set_size(w->pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(w->pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(w->pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(w->pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(w->pill, 36, 0);
    lv_obj_set_style_border_width(w->pill, 1, 0);
    lv_obj_set_style_border_opa(w->pill, 90, 0);
    lv_obj_set_style_pad_left(w->pill, KS(16), 0);
    lv_obj_set_style_pad_right(w->pill, KS(18), 0);
    lv_obj_set_style_pad_ver(w->pill, KS(9), 0);
    lv_obj_set_style_pad_column(w->pill, KS(8), 0);
    lv_obj_set_style_margin_bottom(w->pill, KS(4), 0);
    w->pill_dot = kit_plain(w->pill);
    lv_obj_set_size(w->pill_dot, KS(8), KS(8));
    lv_obj_set_style_radius(w->pill_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(w->pill_dot, LV_OPA_COVER, 0);
    w->pill_lbl = lv_label_create(w->pill);
    lv_obj_set_style_text_font(w->pill_lbl, &brew_font_caps, 0);
    lv_obj_set_style_text_letter_space(w->pill_lbl, KS(3), 0);

    /* QR code on a light card (phones read dark-on-light codes most reliably) */
    w->qr_box = kit_plain(w->root);
    lv_obj_set_size(w->qr_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(w->qr_box, lv_color_hex(0xeef2f5), 0);
    lv_obj_set_style_bg_opa(w->qr_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(w->qr_box, KS(14), 0);
    lv_obj_set_style_pad_all(w->qr_box, KS(10), 0);
#if LV_USE_QRCODE
    w->qr = lv_qrcode_create(w->qr_box);
    lv_qrcode_set_size(w->qr, qr_px);
    lv_qrcode_set_dark_color(w->qr, lv_color_hex(0x0b0e11));
    lv_qrcode_set_light_color(w->qr, lv_color_hex(0xeef2f5));
#else
    LV_UNUSED(qr_px);
#endif

    w->icon = wifi_label(w->root, &brew_font_menu, 0xeef2f5);
    w->spin = lv_spinner_create(w->root);
    lv_obj_set_size(w->spin, KS(54), KS(54));
    lv_obj_set_style_arc_width(w->spin, KS(3), LV_PART_MAIN);
    lv_obj_set_style_arc_color(w->spin, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(w->spin, 30, LV_PART_MAIN);
    lv_obj_set_style_arc_width(w->spin, KS(3), LV_PART_INDICATOR);
    lv_obj_remove_flag(w->spin, LV_OBJ_FLAG_CLICKABLE);

    w->title = wifi_label(w->root, &brew_font_status, 0xeef2f5);
    w->line1 = wifi_label(w->root, &brew_font_small, 0xa9b3bd);
    w->line2 = wifi_label(w->root, &brew_font_small, 0xeef2f5);
    lv_obj_set_width(w->line1, wd - KS(40));
    lv_label_set_long_mode(w->line1, LV_LABEL_LONG_WRAP);
    w->foot = wifi_label(w->root, &brew_font_caps, 0x66717c);
    lv_obj_set_style_text_letter_space(w->foot, KS(3), 0);
    lv_obj_set_style_margin_top(w->foot, KS(14), 0);
}

void kit_wifi_update(kit_wifi_t * w, const brew_model_t * m)
{
    brew_wifi_text_t t;
    brew_model_wifi_text(m, &t);
    const brew_wifi_screen_t s = brew_model_wifi_screen(m);
    const brew_vstate_t vs = brew_model_vstate(m);
    const lv_color_t hi = lv_color_hex(kit_line[vs]);

    kit_set_text(w->pill_lbl, t.pill);
    kit_set_text_color(w->pill_lbl, hi);
    kit_set_bg_color(w->pill_dot, hi);
    kit_set_bg_color(w->pill, lv_color_hex(kit_base[vs]));
    if(!kit_col_eq(lv_obj_get_style_border_color(w->pill, 0), hi)) lv_obj_set_style_border_color(w->pill, hi, 0);

    kit_set_hidden(w->qr_box, t.qr[0] == '\0');
#if LV_USE_QRCODE
    if(t.qr[0] && strcmp(t.qr, w->qr_text) != 0) {   /* re-encode only when the network changes */
        snprintf(w->qr_text, sizeof(w->qr_text), "%s", t.qr);
        lv_qrcode_update(w->qr, w->qr_text, (uint32_t)strlen(w->qr_text));
    }
#endif
    const char * icon = s == BREW_WIFI_SCREEN_PHONE ? BREW_ICON_PHONE : s == BREW_WIFI_SCREEN_DONE ? BREW_ICON_CHECK : "";
    kit_set_text(w->icon, icon);
    kit_set_text_color(w->icon, s == BREW_WIFI_SCREEN_DONE ? hi : lv_color_hex(0xeef2f5));
    kit_set_hidden(w->icon, icon[0] == '\0');
    kit_set_hidden(w->spin, s != BREW_WIFI_SCREEN_TRYING);
    if(s == BREW_WIFI_SCREEN_TRYING) lv_obj_set_style_arc_color(w->spin, hi, LV_PART_INDICATOR);

    kit_set_text(w->title, t.title);
    kit_set_text_color(w->title, s == BREW_WIFI_SCREEN_FAILED ? hi : lv_color_hex(0xeef2f5));
    kit_set_text(w->line1, t.line1);
    kit_set_hidden(w->line1, t.line1[0] == '\0');
    kit_set_text(w->line2, t.line2);
    kit_set_hidden(w->line2, t.line2[0] == '\0');
    kit_set_text(w->foot, t.foot);
}

/* ------------------------------------------------------------------ list rows */

const char * kit_icon(brew_icon_t ic)
{
    switch(ic) {
        case BREW_IC_BEER:      return BREW_ICON_BEER;
        case BREW_IC_SNOWFLAKE: return BREW_ICON_SNOWFLAKE;
        case BREW_IC_PAUSE:     return BREW_ICON_PAUSE;
        case BREW_IC_PLAY:      return BREW_ICON_PLAY;
        case BREW_IC_SETTINGS:  return BREW_ICON_SETTINGS;
        case BREW_IC_BACK:      return BREW_ICON_BACK;
        case BREW_IC_TEMP:      return BREW_ICON_TEMP;
        case BREW_IC_WIFI:      return BREW_ICON_WIFI;
        case BREW_IC_FLAME:     return BREW_ICON_FLAME;
        case BREW_IC_PROFILE:   return BREW_ICON_PROFILE;
        case BREW_IC_CALENDAR:  return BREW_ICON_CALENDAR;
        default:                return "";
    }
}

void kit_rows_create(kit_rows_t * r, lv_obj_t * parent, int32_t row_h, int32_t gap, int32_t dot_px, bool left)
{
    memset(r, 0, sizeof(*r));
    for(int i = 0; i < BREW_LIST_MAX; i++) {
        lv_obj_t * it = kit_plain(parent);
        lv_obj_set_size(it, LV_SIZE_CONTENT, row_h);
        lv_obj_set_flex_flow(it, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(it, left ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(it, gap, 0);
        r->ico[i] = lv_label_create(it);
        lv_obj_set_style_text_color(r->ico[i], lv_color_white(), 0);
        lv_obj_set_style_text_opa(r->ico[i], 230, 0);
        r->txt[i] = lv_label_create(it);
        lv_obj_set_style_text_color(r->txt[i], lv_color_white(), 0);
        r->val[i] = lv_label_create(it);   /* "3 min", "°F": quieter than the label */
        lv_obj_set_style_text_font(r->val[i], &brew_font_menu, 0);
        lv_obj_set_style_text_color(r->val[i], lv_color_hex(0xa9b3bd), 0);
        r->mark[i] = lv_label_create(it);  /* the chosen option: a check */
        lv_obj_set_style_text_font(r->mark[i], &brew_font_menu, 0);
        lv_label_set_text(r->mark[i], BREW_ICON_CHECK);
        r->dot[i] = kit_plain(it);         /* the running mode: a dot */
        lv_obj_set_size(r->dot[i], dot_px, dot_px);
        lv_obj_set_style_radius(r->dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r->dot[i], LV_OPA_COVER, 0);
        r->item[i] = it;
    }
}

void kit_rows_update(kit_rows_t * r, const brew_model_t * m)
{
    const lv_color_t hi = lv_color_hex(kit_line[brew_model_vstate(m)]);
    r->count = brew_model_list_count(m);
    for(int i = 0; i < BREW_LIST_MAX; i++) {
        if(i >= r->count) {
            kit_set_hidden(r->item[i], true);
            continue;
        }
        brew_list_item_t it;
        brew_model_list_item(m, i, &it);
        const bool sel = i == m->list_idx;
        const lv_font_t * f = sel ? &brew_font_menu_bold : &brew_font_menu;
        const char * ic = kit_icon(it.icon);
        kit_set_text(r->ico[i], ic);
        kit_set_hidden(r->ico[i], ic[0] == '\0');
        kit_set_font(r->ico[i], f);
        kit_set_text(r->txt[i], it.label);
        kit_set_font(r->txt[i], f);
        kit_set_text_opa(r->txt[i], it.back ? 200 : LV_OPA_COVER);
        kit_set_text(r->val[i], it.value);
        kit_set_hidden(r->val[i], it.value[0] == '\0');
        kit_set_text_color(r->val[i], sel ? hi : lv_color_hex(0xa9b3bd));
        kit_set_hidden(r->mark[i], it.mark != BREW_MARK_CHOSEN);
        kit_set_text_color(r->mark[i], hi);
        kit_set_hidden(r->dot[i], it.mark != BREW_MARK_ACTIVE);
        kit_set_bg_color(r->dot[i], hi);
    }
}

/* ------------------------------------------------------------------ value editor */

void kit_edit_create(kit_edit_t * e, lv_obj_t * parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    memset(e, 0, sizeof(*e));
    e->root = kit_plain(parent);
    lv_obj_set_pos(e->root, x, y);
    lv_obj_set_size(e->root, w, h);
    lv_obj_set_flex_flow(e->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(e->root, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(e->root, KS(8), 0);
    lv_obj_set_style_opa(e->root, LV_OPA_TRANSP, 0);

    e->title = wifi_label(e->root, &brew_font_caps, 0x66717c);
    lv_obj_set_style_text_letter_space(e->title, KS(3), 0);
    lv_obj_t * row = kit_plain(e->root);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, KS(8), 0);
    e->value = wifi_label(row, &brew_font_num_big, 0xffffff);
    e->unit = wifi_label(row, &brew_font_status, 0xa9b3bd);
    lv_obj_set_style_pad_bottom(e->unit, KS(22), 0);
    e->explain = wifi_label(e->root, &brew_font_small, 0xa9b3bd);
    lv_obj_set_width(e->explain, w - KS(60));
    lv_label_set_long_mode(e->explain, LV_LABEL_LONG_WRAP);
    e->foot = wifi_label(e->root, &brew_font_caps, 0x66717c);
    lv_obj_set_style_text_letter_space(e->foot, KS(3), 0);
    lv_obj_set_style_margin_top(e->foot, KS(18), 0);
}

void kit_edit_update(kit_edit_t * e, const brew_model_t * m)
{
    brew_edit_text_t t;
    brew_model_edit_text(m, &t);
    kit_set_text(e->title, t.title);
    kit_set_text(e->value, t.value);
    kit_set_text(e->unit, t.unit);
    kit_set_text(e->explain, t.explain);
    kit_set_text(e->foot, t.foot);
}
