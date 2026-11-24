/**
 * layout_square.c - a square panel with three keys on its right edge (up, select, down).
 *
 * Same model, same rules as the round dial (brew_model.c); only the drawing differs:
 *   - the ring becomes a horizontal target gauge under the temperature,
 *   - the graph runs along the bottom edge, full width,
 *   - a key rail on the right edge sits next to the physical keys and says what they do now
 *     (chevrons dim at the end of a range; the select label comes from the model),
 *   - the menu is a vertical wheel whose next item is *above*, so the up key moves up.
 *
 * Layer stack (bottom -> top): bg, graph (+ halo, dot), v_home (status, digits, gauge),
 * v_menu (lists), edit, wifi, rail.
 *
 * Geometry is authored for a 480 px panel and scaled: SQ(x) px.
 */
#include "layout_kit.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DISP     KIT_DISP
#define SQF(x)   ((float)(x) * (float)DISP / 480.0f)
#define SQ(x)    ((int32_t)lroundf(SQF(x)))

#define PAD      SQ(36)             /* left margin */
#define RAIL_X   SQ(450)            /* centre line of the key rail */
#define KEY_DY   SQ(84)             /* up / down keys sit this far above / below the centre */
#define GAUGE_Y  SQ(276)            /* target gauge track (top edge) */
#define GAUGE_W  SQ(372)
#define GAUGE_H  SQ(4)
#define G_Y0     SQ(222)            /* top of the graph canvas (the history view's plot is taller) */
#define KNOB_D   SQ(16)
#define PLOT_YT_HOME SQF(352)
#define ROW_DY   SQ(66)             /* menu row pitch */

static const brew_model_t * M;

static struct {
    brew_vstate_t vs;
    bool vs_valid;

    lv_obj_t * root;
    kit_bg_t bg;
    kit_graph_t g;

    /* home */
    lv_obj_t * v_home;
    lv_obj_t * st_dot;
    lv_obj_t * st_lbl;
    kit_numview_t big;
    lv_obj_t * sub_lbl;
    lv_obj_t * ticks;
    lv_obj_t * track;
    lv_obj_t * fill;
    lv_obj_t * knob;
    lv_obj_t * caption;

    /* menu */
    lv_obj_t * v_menu;
    lv_obj_t * pill;
    kit_rows_t rows;
    lv_obj_t * list_title;
    lv_obj_t * list_note;
    int32_t wheel_pos;       /* selected row * 256, animated */

    /* history */
    kit_wifi_t wifi;
    kit_edit_t edit;

    /* key rail */
    lv_obj_t * rail;
    lv_obj_t * up;
    lv_obj_t * down;
    lv_obj_t * sel_dot;
    lv_obj_t * sel_lbl;

    lv_timer_t * graph_t;
} U;

/* chevrons: lv_line keeps a pointer to its points */
static lv_point_precise_t s_up_pts[3], s_down_pts[3];

static float frac_t(float f)
{
    float v = (f - BREW_T_MIN) / (BREW_T_MAX - BREW_T_MIN);
    return v < 0.002f ? 0.002f : (v > 0.998f ? 0.998f : v);
}

static int32_t gauge_x(float f) { return PAD + (int32_t)lroundf((float)GAUGE_W * frac_t(f)); }

static void graph_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    if(U.g.dirty && lv_tick_elaps(U.g.last_ms) >= KIT_GRAPH_REDRAW_MS) kit_graph_render(&U.g, M);
}

/* ------------------------------------------------------------------ refresh pieces */

static void update_gauge(void)
{
    const float target = brew_model_shown_target(M);
    const int32_t xa = gauge_x(M->temp_f), xb = gauge_x(target);
    const int32_t lo = xa < xb ? xa : xb, hi = xa < xb ? xb : xa;
    lv_obj_set_pos(U.fill, lo, GAUGE_Y);
    lv_obj_set_width(U.fill, hi - lo + 1);
    kit_set_bg_color(U.fill, lv_color_hex(kit_line[brew_model_vstate(M)]));
    const lv_opa_t fo = M->setting ? LV_OPA_COVER : 179;
    if(lv_obj_get_style_bg_opa(U.fill, 0) != fo) lv_obj_set_style_bg_opa(U.fill, fo, 0);

    lv_obj_set_pos(U.knob, xb - KNOB_D / 2, GAUGE_Y + GAUGE_H / 2 - KNOB_D / 2);

    char t[16], s[40];
    brew_model_fmt_temp(M, t, sizeof(t), M->setting ? M->temp_f : target);
    snprintf(s, sizeof(s), "%s %s\xC2\xB0", M->setting ? "Now" : "Target", t);
    kit_set_text(U.caption, s);
}

static void update_rail(void)
{
    kit_set_text(U.sel_lbl, brew_model_press_hint(M));
    kit_set_bg_color(U.sel_dot, lv_color_hex(kit_line[brew_model_vstate(M)]));
    /* up = +1 (warmer / next / newer), down = -1 */
    const lv_opa_t on = 210, off = 46;
    const lv_opa_t ou = brew_model_can_step(M, 1) ? on : off, od = brew_model_can_step(M, -1) ? on : off;
    if(lv_obj_get_style_line_opa(U.up, 0) != ou) lv_obj_set_style_line_opa(U.up, ou, 0);
    if(lv_obj_get_style_line_opa(U.down, 0) != od) lv_obj_set_style_line_opa(U.down, od, 0);
}

static void update_menu_items(void)
{
    kit_rows_update(&U.rows, M);
    const char * title = brew_model_list_title(M);
    kit_set_text(U.list_title, title);
    kit_set_hidden(U.list_title, title[0] == '\0');
    char note[64];
    brew_model_list_note(M, note, sizeof(note));
    kit_set_text(U.list_note, note);
    kit_set_hidden(U.list_note, note[0] == '\0');
}

static void layout_wheel(void)
{
    /* The selected row sits in the pill at the centre; the next item is above it, so the up
     * key brings the row above into the pill. Neighbours fade with distance. */
    const float pos = (float)U.wheel_pos / 256.0f;
    for(int i = 0; i < BREW_LIST_MAX; i++) {
        const float o = (float)i - pos, ao = fabsf(o);
        float op = 1.0f - 0.42f * ao;
        if(op < 0.0f) op = 0.0f;
        op = op * op * (3.0f - 2.0f * op);
        lv_obj_align(U.rows.item[i], LV_ALIGN_LEFT_MID, SQ(52), (int32_t)lroundf(-o * (float)ROW_DY));
        lv_obj_set_style_opa(U.rows.item[i], (lv_opa_t)lroundf(op * 255.0f), 0);
        kit_set_hidden(U.rows.item[i], op <= 0.01f || i >= U.rows.count);
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
    lv_anim_set_duration(&a, 280);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void update_visibility(void)
{
    const bool home = M->view == BREW_VIEW_HOME;
    kit_fade_to(U.v_home, home ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.v_menu, M->view == BREW_VIEW_LIST ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.edit.root, M->view == BREW_VIEW_EDIT ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_set_hidden(U.v_menu, M->view != BREW_VIEW_LIST && lv_obj_get_style_opa(U.v_menu, 0) == 0 && !lv_anim_get(U.v_menu, kit_opa_exec));
    kit_set_hidden(U.edit.root, M->view != BREW_VIEW_EDIT && lv_obj_get_style_opa(U.edit.root, 0) == 0 && !lv_anim_get(U.edit.root, kit_opa_exec));
    const bool wifi = M->view == BREW_VIEW_WIFI;
    kit_fade_to(U.wifi.root, wifi ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_set_hidden(U.wifi.root, !wifi && lv_obj_get_style_opa(U.wifi.root, 0) == 0 && !lv_anim_get(U.wifi.root, kit_opa_exec));
    kit_fade_to(U.ticks, home && M->setting ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);

    const lv_opa_t g = wifi || M->view == BREW_VIEW_EDIT ? LV_OPA_TRANSP : M->view == BREW_VIEW_LIST ? 64 : (M->setting ? 102 : LV_OPA_COVER);
    kit_fade_to(U.g.canvas, g, BREW_FADE_MS);
    kit_fade_to(U.g.halo, g, BREW_FADE_MS);
    kit_fade_to(U.g.dot, g, BREW_FADE_MS);
}

static void refresh(bool animate)
{
    const brew_vstate_t vs = brew_model_vstate(M);
    if(!U.vs_valid || vs != U.vs) {
        kit_bg_show(&U.bg, vs, animate && U.vs_valid);
        if(!U.vs_valid || kit_line[vs] != kit_line[U.vs]) U.g.dirty = true;
        U.vs = vs;
        U.vs_valid = true;
    }
    if(U.g.dirty) kit_graph_render(&U.g, M);

    char t[24], s[48];
    const lv_color_t white = lv_color_white();
    const lv_color_t cool = lv_color_hex(KIT_COOL_TEXT);

    if(M->setting) {
        kit_set_text(U.st_lbl, "Target");
        kit_set_text_color(U.st_lbl, white);
        kit_set_text_opa(U.st_lbl, 179);
        kit_set_hidden(U.st_dot, true);
        kit_nv_set_color(&U.big, white);
        kit_set_text(U.sub_lbl, "Up / down to adjust");
        brew_model_fmt_temp(M, t, sizeof(t), M->draft_f);
        kit_nv_set_unit(&U.big, brew_model_unit(M));
        kit_nv_set(&U.big, t, animate);
    }
    else {
        brew_model_status_text(M, s, sizeof(s));
        kit_set_text(U.st_lbl, s);
        const bool is_cool = vs == BREW_VS_COOL;
        kit_set_text_color(U.st_lbl, is_cool ? cool : white);
        kit_set_text_opa(U.st_lbl, is_cool ? LV_OPA_COVER : 224);
        kit_set_bg_color(U.st_dot, is_cool ? cool : white);
        kit_set_hidden(U.st_dot, vs == BREW_VS_IDLE || vs == BREW_VS_PAUSE);
        kit_nv_set_color(&U.big, is_cool ? cool : white);
        brew_model_batch_text(M, s, sizeof(s));
        kit_set_text(U.sub_lbl, s);
        brew_model_fmt_temp(M, t, sizeof(t), M->temp_f);
        kit_nv_set_unit(&U.big, brew_model_unit(M));
        kit_nv_set(&U.big, t, animate);
    }

    if(M->view == BREW_VIEW_LIST) update_menu_items();
    if(M->view == BREW_VIEW_EDIT) kit_edit_update(&U.edit, M);
    if(M->view == BREW_VIEW_WIFI) kit_wifi_update(&U.wifi, M);
    update_gauge();
    update_rail();
    update_visibility();
    kit_graph_place_marker(&U.g, M);
}

/* ------------------------------------------------------------------ construction */

static lv_obj_t * label(lv_obj_t * parent, const lv_font_t * f, lv_opa_t opa)
{
    lv_obj_t * l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    return l;
}

static lv_obj_t * full_view(lv_obj_t * parent)
{
    lv_obj_t * v = kit_plain(parent);
    lv_obj_set_size(v, DISP, DISP);
    return v;
}

static lv_obj_t * chevron(lv_obj_t * parent, lv_point_precise_t * pts, bool up, int32_t cy)
{
    const int32_t w = SQ(20), h = SQ(10), lw = SQ(3);
    const lv_value_precise_t fw = (lv_value_precise_t)w, fh = (lv_value_precise_t)h, z = 0;
    pts[0].x = z;      pts[0].y = up ? fh : z;
    pts[1].x = fw / 2; pts[1].y = up ? z : fh;
    pts[2].x = fw;     pts[2].y = up ? fh : z;
    lv_obj_t * l = lv_line_create(parent);
    lv_line_set_points(l, pts, 3);
    lv_obj_set_style_line_width(l, lw, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_style_line_color(l, lv_color_white(), 0);
    lv_obj_set_pos(l, RAIL_X - w / 2, cy - h / 2);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void tap_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    brew_ui_press();   /* on a touch panel, a tap is the select key */
}

static void square_create(lv_obj_t * screen, const brew_model_t * m)
{
    memset(&U, 0, sizeof(U));
    M = m;

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    U.root = kit_plain(screen);
    lv_obj_set_size(U.root, DISP, DISP);
    lv_obj_center(U.root);
    lv_obj_add_flag(U.root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.root, tap_cb, LV_EVENT_CLICKED, NULL);

    kit_bg_create(&U.bg, U.root);

    /* graph along the bottom, clear of the key rail */
    U.g.cx = 0;
    U.g.cy = G_Y0;
    U.g.cw = DISP;
    U.g.ch = DISP - G_Y0;
    U.g.x0 = SQF(36);
    U.g.x1 = SQF(408);
    U.g.yt = PLOT_YT_HOME;
    U.g.yb = SQF(446);
    U.g.line_w = SQF(3.4f);
    U.g.halo_d = SQ(22);
    U.g.dot_d = SQ(11);


    kit_graph_create(&U.g, U.root);

    /* ---- home ---- */
    U.v_home = full_view(U.root);
    lv_obj_t * st_row = kit_plain(U.v_home);
    lv_obj_set_size(st_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st_row, SQ(10), 0);
    lv_obj_set_pos(st_row, PAD, SQ(30));
    U.st_dot = kit_plain(st_row);
    lv_obj_set_size(U.st_dot, SQ(9), SQ(9));
    lv_obj_set_style_radius(U.st_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.st_dot, LV_OPA_COVER, 0);
    kit_breathe(U.st_dot);
    U.st_lbl = label(st_row, &brew_font_status, LV_OPA_COVER);

    kit_nv_create(&U.big, U.v_home, &brew_font_num_big, &brew_font_deg_big, SQ(38), SQ(7));
    lv_obj_set_pos(U.big.box, PAD - SQ(5), SQ(78));

    U.sub_lbl = label(U.v_home, &brew_font_small, 140);
    lv_obj_set_pos(U.sub_lbl, PAD, SQ(218));

    U.ticks = lv_scale_create(U.v_home);
    lv_obj_remove_style_all(U.ticks);
    lv_obj_set_size(U.ticks, GAUGE_W, SQ(14));
    lv_obj_set_pos(U.ticks, PAD, GAUGE_Y - SQ(18));
    lv_scale_set_mode(U.ticks, LV_SCALE_MODE_HORIZONTAL_TOP);
    lv_scale_set_range(U.ticks, (int32_t)BREW_T_MIN, (int32_t)BREW_T_MAX);
    lv_scale_set_total_tick_count(U.ticks, (uint32_t)(BREW_T_MAX - BREW_T_MIN) + 1);
    lv_scale_set_major_tick_every(U.ticks, 5);
    lv_scale_set_label_show(U.ticks, false);
    lv_obj_set_style_line_width(U.ticks, 0, LV_PART_MAIN);
    lv_obj_set_style_line_color(U.ticks, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_line_opa(U.ticks, 64, LV_PART_ITEMS);
    lv_obj_set_style_line_width(U.ticks, 1, LV_PART_ITEMS);
    lv_obj_set_style_length(U.ticks, SQ(5), LV_PART_ITEMS);
    lv_obj_set_style_line_color(U.ticks, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_line_opa(U.ticks, 153, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(U.ticks, SQ(2), LV_PART_INDICATOR);
    lv_obj_set_style_length(U.ticks, SQ(10), LV_PART_INDICATOR);
    lv_obj_set_style_opa(U.ticks, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(U.ticks, LV_OBJ_FLAG_CLICKABLE);

    U.track = kit_plain(U.v_home);
    lv_obj_set_size(U.track, GAUGE_W, GAUGE_H);
    lv_obj_set_pos(U.track, PAD, GAUGE_Y);
    lv_obj_set_style_radius(U.track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(U.track, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(U.track, 26, 0);

    U.fill = kit_plain(U.v_home);
    lv_obj_set_height(U.fill, GAUGE_H);
    lv_obj_set_style_radius(U.fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.fill, 179, 0);

    U.knob = kit_plain(U.v_home);
    lv_obj_set_size(U.knob, KNOB_D, KNOB_D);
    lv_obj_set_style_radius(U.knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(U.knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(U.knob, LV_OPA_COVER, 0);

    U.caption = label(U.v_home, &brew_font_small, 140);
    lv_obj_set_pos(U.caption, PAD, GAUGE_Y + SQ(14));

    /* ---- menu ---- */
    U.v_menu = full_view(U.root);
    lv_obj_set_style_opa(U.v_menu, LV_OPA_TRANSP, 0);
    U.pill = kit_plain(U.v_menu);
    lv_obj_set_size(U.pill, SQ(370), SQ(58));
    lv_obj_align(U.pill, LV_ALIGN_LEFT_MID, SQ(28), 0);
    lv_obj_set_style_radius(U.pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(U.pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(U.pill, 30, 0);
    lv_obj_set_style_border_color(U.pill, lv_color_white(), 0);
    lv_obj_set_style_border_opa(U.pill, 36, 0);
    lv_obj_set_style_border_width(U.pill, 1, 0);
    kit_rows_create(&U.rows, U.v_menu, ROW_DY, SQ(14), SQ(7), true);
    U.list_title = label(U.v_menu, &brew_font_caps, LV_OPA_COVER);
    lv_obj_set_style_text_letter_space(U.list_title, SQ(3), 0);
    lv_obj_set_style_text_color(U.list_title, lv_color_hex(0x66717c), 0);
    lv_obj_set_pos(U.list_title, PAD, SQ(40));
    U.list_note = label(U.v_menu, &brew_font_small, 180);
    lv_obj_set_pos(U.list_note, PAD, SQ(64));

    /* ---- value editor, left of the key rail ---- */
    kit_edit_create(&U.edit, U.root, 0, 0, RAIL_X - SQ(40), DISP);

    /* ---- WiFi setup view, left of the key rail ---- */
    kit_wifi_create(&U.wifi, U.root, 0, 0, RAIL_X - SQ(40), DISP, SQ(150));

    /* ---- key rail, beside the physical keys ---- */
    U.rail = full_view(U.root);
    U.up = chevron(U.rail, s_up_pts, true, DISP / 2 - KEY_DY);
    U.down = chevron(U.rail, s_down_pts, false, DISP / 2 + KEY_DY);
    U.sel_dot = kit_plain(U.rail);
    lv_obj_set_size(U.sel_dot, SQ(12), SQ(12));
    lv_obj_set_pos(U.sel_dot, RAIL_X - SQ(6), DISP / 2 - SQ(6));
    lv_obj_set_style_radius(U.sel_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.sel_dot, LV_OPA_COVER, 0);
    U.sel_lbl = label(U.rail, &brew_font_small, 200);
    lv_obj_align(U.sel_lbl, LV_ALIGN_RIGHT_MID, -(DISP - RAIL_X) - SQ(16), 0);

    U.graph_t = lv_timer_create(graph_timer_cb, 50, NULL);    /* checks; renders per KIT_GRAPH_REDRAW_MS */

    U.wheel_pos = m->list_idx * 256;
    update_menu_items();
    layout_wheel();
    refresh(false);
}

/* ------------------------------------------------------------------ model changes -> drawing */

static void square_update(const brew_model_t * m, uint32_t ch)
{
    M = m;
    const bool animate = (ch & BREW_CH_ANIMATE) != 0;

    if(ch & BREW_CH_VIEW) {
        if(M->view == BREW_VIEW_LIST) {
            update_menu_items();
            lv_anim_delete(&U.wheel_pos, wheel_exec);
            U.wheel_pos = M->list_idx * 256;
            layout_wheel();
        }
    }
    if(ch & (BREW_CH_GRAPH | BREW_CH_GRAPH_LAZY)) U.g.dirty = true;
    if(ch & BREW_CH_MENU_MOVE) {
        update_menu_items();
        update_rail();
        wheel_animate_to(M->list_idx);
    }
    if(!(ch & BREW_CH_REFRESH)) return;

    if((ch & BREW_CH_TEMP) && !(ch & BREW_CH_GRAPH)) {
        if(kit_graph_live_moved(&U.g, M->temp_f)) U.g.dirty = true;
        const bool g = U.g.dirty;
        U.g.dirty = false;    /* graph_t redraws the live point at its own pace */
        refresh(animate);
        U.g.dirty = g;
        return;
    }
    refresh(animate);
}

static void square_destroy(void)
{
    lv_anim_delete(&U.wheel_pos, wheel_exec);
    lv_timer_delete(U.graph_t);
    lv_obj_delete(U.root);
    kit_graph_free(&U.g);
    memset(&U, 0, sizeof(U));
}

const brew_layout_t brew_layout_square = {
    .name = "square",
    .create = square_create,
    .update = square_update,
    .destroy = square_destroy,
};
