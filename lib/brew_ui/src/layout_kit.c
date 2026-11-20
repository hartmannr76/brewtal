/**
 * layout_kit.c - drawing pieces shared by the layouts. See layout_kit.h.
 */
#include "layout_kit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static const brew_grad_t s_grad[BREW_VS_N] = {
    {{0x3070b0, 0x20456b, 0x1c2a37, 0x171f27}, {0.0f, 0.42f, 0.78f, 1.0f}}, /* cool  */
    {{0x3070b0, 0x20456b, 0x1c2a37, 0x171f27}, {0.0f, 0.42f, 0.78f, 1.0f}}, /* idle  */
    {{0xff8f3a, 0xc4561a, 0x4a2516, 0x2a1912}, {0.0f, 0.40f, 0.78f, 1.0f}}, /* heat  */
    {{0x8c5ff2, 0x4b2e98, 0x241c3e, 0x18142a}, {0.0f, 0.42f, 0.78f, 1.0f}}, /* crash */
    {{0x5f6871, 0x383f46, 0x22272c, 0x1a1d21}, {0.0f, 0.45f, 0.80f, 1.0f}}, /* pause */
};
const uint32_t kit_line[BREW_VS_N] = { 0x7cd4ff, 0x7cd4ff, 0xffcf8a, 0xd8bcff, 0xc3cad1 };

/* ------------------------------------------------------------------ setters */

bool kit_col_eq(lv_color_t a, lv_color_t b) { return a.red == b.red && a.green == b.green && a.blue == b.blue; }

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

static uint16_t * s_bg_cache[4];   /* one pre-rendered background per distinct gradient */

static int grad_id(brew_vstate_t vs)
{
    switch(vs) {
        case BREW_VS_HEAT:  return 1;
        case BREW_VS_CRASH: return 2;
        case BREW_VS_PAUSE: return 3;
        default:            return 0;   /* cooling and holding share the blue gradient */
    }
}
static const brew_vstate_t s_gid_vs[4] = { BREW_VS_COOL, BREW_VS_HEAT, BREW_VS_CRASH, BREW_VS_PAUSE };

static uint16_t * bg_get(int gid)
{
    if(!s_bg_cache[gid]) {
        s_bg_cache[gid] = BREW_UI_ALLOC((size_t)KIT_DISP * KIT_DISP * 2);
        LV_ASSERT_MALLOC(s_bg_cache[gid]);
        brew_bg_render(s_bg_cache[gid], KIT_DISP, KIT_DISP, &s_grad[s_gid_vs[gid]]);
    }
    return s_bg_cache[gid];
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
    for(int g = 0; g < 4; g++) bg_get(g);   /* pre-render now so state changes never stall */
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
    return g->yt + (g->yb - g->yt) * (1.0f - (f - g->g_min) / (g->g_max - g->g_min));
}

void kit_graph_create(kit_graph_t * g, lv_obj_t * parent)
{
    g->buf = BREW_UI_ALLOC((size_t)g->cw * g->ch * 4);
    LV_ASSERT_MALLOC(g->buf);
    memset(g->buf, 0, (size_t)g->cw * g->ch * 4);
    g->canvas = kit_canvas(parent, g->buf, g->cw, g->ch, LV_COLOR_FORMAT_ARGB8888);
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
    int i = m->view == BREW_VIEW_HIST ? m->hist_idx : g->nsrc - 1;
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
    return g->npts == 0 || fabsf(kit_graph_temp_y(g, f) - g->pts[g->npts - 1].y) >= 1.0f || f < g->g_min ||
           f > g->g_max;
}

void kit_graph_render(kit_graph_t * g, const brew_model_t * m)
{
    g->dirty = false;
    g->last_ms = lv_tick_get();
    const int n = brew_model_hist_last(m) + 1;
    if(n < 1) {
        memset(g->buf, 0, (size_t)g->cw * g->ch * 4);
        lv_obj_invalidate(g->canvas);
        g->npts = 0;
        g->nsrc = 0;
        g->npoly = 0;
        kit_graph_place_marker(g, m);
        return;
    }
    const float tg = m->sp_f[m->mode];
    float mn = tg, mx = tg;
    for(int i = 0; i < n; i++) {
        const float v = brew_model_hist_val(m, i);
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
    int steps = g->npts > 1 ? (KIT_POLY_MAX - 1) / (g->npts - 1) : 1;
    if(steps > KIT_POLY_STEPS) steps = KIT_POLY_STEPS;
    if(steps < 1) steps = 1;
    g->npoly = g->npts < 2 ? 0 : brew_curve_sample(g->pts, g->npts, g->poly, KIT_POLY_MAX, steps);

    const uint32_t lc = kit_line[brew_model_vstate(m)];
    /* the fill works in parent rows; shift x into the canvas (a no-op for full-width canvases) */
    if(g->cx != 0)
        for(int i = 0; i < g->npoly; i++) g->poly[i].x -= (float)g->cx;
    brew_graph_fill(g->buf, g->cw, g->ch, g->cy, g->poly, g->npoly, lc, 0.38f, 0.02f);

    lv_layer_t layer;
    lv_canvas_init_layer(g->canvas, &layer);

    /* setpoint reference: dotted, 22% white */
    const float sy = g->yt + (g->yb - g->yt) * (1.0f - (tg - mn) / (mx - mn)) - (float)g->cy;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_white();
    d.opa = 56;
    d.width = 1;
    d.dash_width = 2;
    d.dash_gap = 6;
    d.p1.x = 0;
    d.p1.y = sy;
    d.p2.x = (lv_value_precise_t)g->cw;
    d.p2.y = sy;
    lv_draw_line(&layer, &d);

    /* the curve */
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(lc);
    d.opa = LV_OPA_COVER;
    d.width = (int32_t)lroundf(g->line_w);
    d.round_start = 1;
    d.round_end = 1;
    for(int i = 0; i < g->npoly - 1; i++) {
        d.p1.x = g->poly[i].x;
        d.p1.y = g->poly[i].y - (float)g->cy;
        d.p2.x = g->poly[i + 1].x;
        d.p2.y = g->poly[i + 1].y - (float)g->cy;
        lv_draw_line(&layer, &d);
    }
    lv_canvas_finish_layer(g->canvas, &layer);
    lv_obj_invalidate(g->canvas);

    kit_set_bg_color(g->halo, lv_color_hex(lc));
    kit_graph_place_marker(g, m);
}
