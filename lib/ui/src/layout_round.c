/**
 * layout_round.c - the round dial layout (LVGL 9.3), "smoked glass": a near-black face lit by
 * the state colour, a status pill, thin tinted numerals and a glowing setpoint ring.
 *
 * Draws a brew_model_t (brew_model.h); owns no state of its own except presentation state
 * (objects, animations, the rendered graph). Selected through brew_layout_round.
 *
 * Layer stack (bottom -> top):
 *   bg[0], bg[1]   RGB565 canvases, smoked glass lit by the state colour; cross-faded on change
 *   graph          ARGB8888 canvas with the history line, in the model's graph style (dimmable)
 *   halo, dot      "now"/scrub marker on the curve
 *   ticks5         lv_scale, a tick every 5 degrees (brighter every 10)
 *   ticks          lv_scale, every degree, only visible while adjusting
 *   arc_glow, arc  ring: hairline track; a glowing arc spans the reading to the target ("how far
 *                  to go"), hidden while adjusting
 *   now, target    on the ring: a small dot at the reading, a hollow white ring at the target
 *   tgt_lbl        the target's number just inside the ring
 *   v_home / v_menu (lists) / edit / wifi   content views, cross-faded
 *
 * Geometry is authored in the mockup's units and scaled:
 *   DS(x)  SVG viewBox units (240 = full diameter)
 *   DC(x)  CSS px of the mockup screen (276 = full diameter)
 */
#include "layout_kit.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DISP      KIT_DISP
#define DS(x)     ((float)(x) * (float)DISP / 240.0f)
#define DC(x)     ((int32_t)lroundf((float)(x) * (float)DISP / 276.0f))

#define A0        135.0f                   /* ring: BREW_T_MIN at 135 deg, sweeping 270 deg */
#define SWEEP     270.0f
#define RING_R    DS(113)
#define G_Y0      ((int32_t)DS(146))       /* top of graph canvas */
#define G_H       (DISP - G_Y0)

/* ------------------------------------------------------------------ */
/* State: the model it draws, plus presentation state only            */
/* ------------------------------------------------------------------ */
static const brew_model_t * M;

static struct {
    brew_vstate_t vs;        /* last drawn visual state */
    bool vs_valid;

    /* objects */
    lv_obj_t * root;
    kit_bg_t bg;
    kit_graph_t g;

    lv_obj_t * ticks5;
    lv_obj_t * ticks;
    lv_obj_t * arc_glow;
    lv_obj_t * arc;
    lv_obj_t * now;
    lv_obj_t * target;
    lv_obj_t * tgt_lbl;

    lv_obj_t * v_home;
    lv_obj_t * v_menu;
    kit_wifi_t wifi;
    kit_edit_t edit;
    lv_obj_t * list_title;
    lv_obj_t * list_note;
    lv_obj_t * pill;
    lv_obj_t * st_dot;
    lv_obj_t * st_lbl;
    lv_obj_t * sub_lbl;
    kit_numview_t big;
    kit_rows_t rows;
    int32_t wheel_pos; /* selected row * 256, animated */

    lv_timer_t * graph_t;
} U;

static float frac_t(float f)
{
    float v = (f - BREW_T_MIN) / (BREW_T_MAX - BREW_T_MIN);
    return v < 0.002f ? 0.002f : (v > 0.998f ? 0.998f : v);
}

static float norm_deg(float a)
{
    while(a >= 360.0f) a -= 360.0f;
    while(a < 0.0f) a += 360.0f;
    return a;
}

static void graph_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    if(U.g.dirty && lv_tick_elaps(U.g.last_ms) >= KIT_GRAPH_REDRAW_MS) kit_graph_render(&U.g, M);
}

/* ------------------------------------------------------------------ */
/* Refresh                                                            */
/* ------------------------------------------------------------------ */

static void ring_place(lv_obj_t * o, float frac, float r)
{
    const float ang = (A0 + SWEEP * frac) * 3.14159265f / 180.0f;
    lv_obj_align(o, LV_ALIGN_CENTER, (int32_t)lroundf(r * cosf(ang)), (int32_t)lroundf(r * sinf(ang)));
}

static void update_ring(void)
{
    if(M->view == BREW_VIEW_EDIT) {   /* the value being edited, as an arc from the scale's start */
        brew_edit_text_t t;
        brew_model_edit_text(M, &t);
        const float f = t.frac < 0.002f ? 0.002f : (t.frac > 1.0f ? 1.0f : t.frac);
        const lv_value_precise_t e0 = (lv_value_precise_t)A0, e1 = (lv_value_precise_t)norm_deg(A0 + SWEEP * f);
        kit_arc_set_angles(U.arc, e0, e1);
        kit_arc_set_angles(U.arc_glow, e0, e1);
        return;
    }
    const float target = M->setting ? M->draft_f : M->sp_f[M->mode];
    const float bt = frac_t(target);
    const float bn = M->temp_known ? frac_t(M->temp_f) : bt;

    /* the arc spans the reading to the target (empty while adjusting or with no reading) */
    const float lo = M->setting ? bt : (bn < bt ? bn : bt), hi_f = M->setting ? bt : (bn < bt ? bt : bn);
    const lv_value_precise_t a0 = (lv_value_precise_t)norm_deg(A0 + SWEEP * lo);
    const lv_value_precise_t a1 = (lv_value_precise_t)norm_deg(A0 + SWEEP * hi_f);
    kit_arc_set_angles(U.arc, a0, a1);
    kit_arc_set_angles(U.arc_glow, a0, a1);

    const lv_color_t ac = lv_color_hex(kit_line[brew_model_vstate(M)]);
    if(!kit_col_eq(lv_obj_get_style_arc_color(U.arc, LV_PART_INDICATOR), ac)) {
        lv_obj_set_style_arc_color(U.arc, ac, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(U.arc_glow, ac, LV_PART_INDICATOR);
        lv_obj_set_style_shadow_color(U.target, ac, 0);
        lv_obj_set_style_shadow_color(U.now, ac, 0);
        lv_obj_set_style_bg_color(U.now, ac, 0);
    }
    kit_set_text_color(U.tgt_lbl, M->setting ? lv_color_white() : ac);

    ring_place(U.target, bt, RING_R);
    ring_place(U.now, bn, RING_R);

    /* the number: whole degrees when the target is whole, tenths while adjusting */
    const float d = brew_model_to_disp(M, target);
    char t[12];
    if(M->setting || fabsf(d - roundf(d)) >= 0.05f) snprintf(t, sizeof(t), "%.1f", (double)d);
    else snprintf(t, sizeof(t), "%d", (int)lroundf(d));
    kit_set_text(U.tgt_lbl, t);
    ring_place(U.tgt_lbl, bt, RING_R - DS(17));
}

static void update_visibility(void)
{
    const bool home = M->view == BREW_VIEW_HOME;
    /* Fades re-render everything under the faded object every frame, so they're kept
     * short (BREW_FADE_MS). The ring/ticks cover nearly the whole screen. */
    kit_fade_to(U.v_home, home ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.v_menu, M->view == BREW_VIEW_LIST ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.edit.root, M->view == BREW_VIEW_EDIT ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    /* hide fully transparent views so they cost nothing and can't be invalidated */
    kit_set_hidden(U.v_menu, M->view != BREW_VIEW_LIST && lv_obj_get_style_opa(U.v_menu, 0) == 0 && !lv_anim_get(U.v_menu, kit_opa_exec));
    kit_set_hidden(U.edit.root, M->view != BREW_VIEW_EDIT && lv_obj_get_style_opa(U.edit.root, 0) == 0 && !lv_anim_get(U.edit.root, kit_opa_exec));
    const bool wifi = M->view == BREW_VIEW_WIFI;
    kit_fade_to(U.wifi.root, wifi ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_set_hidden(U.wifi.root, !wifi && lv_obj_get_style_opa(U.wifi.root, 0) == 0 && !lv_anim_get(U.wifi.root, kit_opa_exec));

    const bool ring = home || M->view == BREW_VIEW_EDIT;   /* the editor shows its value on the ring */
    kit_fade_to(U.arc, ring ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.arc_glow, ring ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.ticks5, home ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.target, home ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);   /* not in the editor */
    kit_fade_to(U.tgt_lbl, home ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.now, home && !M->setting && M->temp_known ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.ticks, home && M->setting ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);

    const lv_opa_t g = wifi || M->view == BREW_VIEW_EDIT ? LV_OPA_TRANSP : M->view == BREW_VIEW_LIST ? 64 : (M->setting ? 102 : LV_OPA_COVER);
    kit_fade_to(U.g.canvas, g, BREW_FADE_MS);
    kit_fade_to(U.g.halo, g, BREW_FADE_MS);
    kit_fade_to(U.g.dot, g, BREW_FADE_MS);
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
    /* Watch-style wheel: the centred row is full strength, neighbours fade out.
     * Depth comes from opacity + weight rather than transform_scale: in LVGL 9.3
     * transformed rows that scroll in from outside their parent render clipped. */
    const float pos = (float)U.wheel_pos / 256.0f;
    for(int i = 0; i < BREW_LIST_MAX; i++) {
        const float o = (float)i - pos, ao = fabsf(o);
        float op = 1.0f - 0.5f * ao;
        if(op < 0.0f) op = 0.0f;
        op = op * op * (3.0f - 2.0f * op); /* smoothstep: crisper falloff */
        lv_obj_align(U.rows.item[i], LV_ALIGN_CENTER, 0, (int32_t)lroundf(o * (float)DC(42)));
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
    lv_anim_set_duration(&a, 320);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}


static void refresh(bool animate)
{
    const brew_vstate_t vs = brew_model_vstate(M);
    if(!U.vs_valid || vs != U.vs) {
        kit_bg_show(&U.bg, vs, animate && U.vs_valid);
        if(!U.vs_valid || kit_line[vs] != kit_line[U.vs]) U.g.dirty = true;  /* line colour changed */
        U.vs = vs;
        U.vs_valid = true;
    }
    if(U.g.dirty) kit_graph_render(&U.g, M);

    char t[24], s[48];
    const lv_color_t white = lv_color_white();
    const lv_color_t hi = lv_color_hex(kit_line[vs]);
    /* the pill: the state colour at 14% with a 35% hairline; white while adjusting */
    const lv_color_t pill = M->setting ? white : lv_color_hex(kit_base[vs]);
    const lv_color_t ink = M->setting ? white : hi;
    kit_set_bg_color(U.pill, pill);
    if(lv_obj_get_style_bg_opa(U.pill, 0) != (M->setting ? 20 : 36)) lv_obj_set_style_bg_opa(U.pill, M->setting ? 20 : 36, 0);
    if(!kit_col_eq(lv_obj_get_style_border_color(U.pill, 0), ink)) lv_obj_set_style_border_color(U.pill, ink, 0);
    brew_model_pill_text(M, s, sizeof(s));
    kit_set_text(U.st_lbl, s);
    kit_set_text_color(U.st_lbl, ink);
    kit_set_bg_color(U.st_dot, ink);
    kit_set_hidden(U.st_dot, M->setting);

    kit_nv_set_color(&U.big, M->setting ? white : hi);
    kit_nv_set_unit(&U.big, brew_model_unit(M));
    brew_model_detail_text(M, s, sizeof(s));
    kit_set_text(U.sub_lbl, s);
    brew_model_temp_text(M, t, sizeof(t));
    kit_nv_set(&U.big, t, animate);

    if(M->view == BREW_VIEW_LIST) update_menu_items();
    if(M->view == BREW_VIEW_EDIT) kit_edit_update(&U.edit, M);
    if(M->view == BREW_VIEW_WIFI) kit_wifi_update(&U.wifi, M);
    update_ring();
    update_visibility();
    kit_graph_place_marker(&U.g, M);
}


/* ------------------------------------------------------------------ */
/* Construction                                                       */
/* ------------------------------------------------------------------ */
static lv_obj_t * make_view(lv_obj_t * parent, int32_t pad_top, int32_t gap)
{
    lv_obj_t * v = kit_plain(parent);
    lv_obj_set_size(v, DISP, DISP);
    lv_obj_set_flex_flow(v, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(v, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(v, pad_top, 0);
    lv_obj_set_style_pad_row(v, gap, 0);
    return v;
}

static void tap_cb(lv_event_t * e)
{
    LV_UNUSED(e);
    brew_ui_press();   /* a tap anywhere is the knob press */
}

static void round_create(lv_obj_t * screen, const brew_model_t * m)
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

    /* graph: oldest sample at the left, inside the round glass */
    U.g.cx = 0;
    U.g.cy = G_Y0;
    U.g.cw = DISP;
    U.g.ch = G_H;
    U.g.x0 = DS(36);
    U.g.x1 = DS(204);
    U.g.yt = DS(158);
    U.g.yb = DS(212);
    U.g.line_w = DS(1.8f);
    U.g.halo_d = (int32_t)lroundf(DS(12));
    U.g.dot_d = (int32_t)lroundf(DS(6));
    U.g.clip_cx = (float)DISP / 2.0f;
    U.g.clip_cy = (float)DISP / 2.0f;
    U.g.clip_r = (float)DISP / 2.0f - DS(2);
    kit_graph_create(&U.g, U.root);

    /* a tick every 5 degrees, brighter every 10, just inside the ring */
    const int32_t t5_d = (int32_t)lroundf((RING_R - DS(3.5f)) * 2.0f);
    U.ticks5 = lv_scale_create(U.root);
    lv_obj_remove_style_all(U.ticks5);
    lv_obj_set_size(U.ticks5, t5_d, t5_d);
    lv_obj_center(U.ticks5);
    lv_scale_set_mode(U.ticks5, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(U.ticks5, (int32_t)BREW_T_MIN, (int32_t)BREW_T_MAX);
    lv_scale_set_total_tick_count(U.ticks5, (uint32_t)((BREW_T_MAX - BREW_T_MIN) / 5.0f) + 1);
    lv_scale_set_major_tick_every(U.ticks5, 2);
    lv_scale_set_label_show(U.ticks5, false);
    lv_scale_set_angle_range(U.ticks5, (uint32_t)SWEEP);
    lv_scale_set_rotation(U.ticks5, (int32_t)A0);
    lv_obj_set_style_arc_width(U.ticks5, 0, LV_PART_MAIN);
    lv_obj_set_style_line_color(U.ticks5, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_line_opa(U.ticks5, 26, LV_PART_ITEMS);
    lv_obj_set_style_line_width(U.ticks5, (int32_t)lroundf(DS(0.75f)), LV_PART_ITEMS);
    lv_obj_set_style_length(U.ticks5, (int32_t)lroundf(DS(2.5f)), LV_PART_ITEMS);
    lv_obj_set_style_line_color(U.ticks5, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_line_opa(U.ticks5, 56, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(U.ticks5, (int32_t)lroundf(DS(0.75f)), LV_PART_INDICATOR);
    lv_obj_set_style_length(U.ticks5, (int32_t)lroundf(DS(2.5f)), LV_PART_INDICATOR);
    lv_obj_remove_flag(U.ticks5, LV_OBJ_FLAG_CLICKABLE);

    /* ticks (only while adjusting) */
    const int32_t tick_d = (int32_t)lroundf(DS(107) * 2.0f);
    U.ticks = lv_scale_create(U.root);
    lv_obj_remove_style_all(U.ticks);
    lv_obj_set_size(U.ticks, tick_d, tick_d);
    lv_obj_center(U.ticks);
    lv_scale_set_mode(U.ticks, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(U.ticks, (int32_t)BREW_T_MIN, (int32_t)BREW_T_MAX);
    lv_scale_set_total_tick_count(U.ticks, (uint32_t)(BREW_T_MAX - BREW_T_MIN) + 1);
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

    /* setpoint ring: a soft glow (a wide faint arc) under a hairline arc */
    const int32_t ring_w = (int32_t)lroundf(DS(1.6f));
    const int32_t glow_w = (int32_t)lroundf(DS(6));
    const int32_t ring_d = (int32_t)lroundf(RING_R * 2.0f) + ring_w;
    U.arc_glow = lv_arc_create(U.root);
    lv_obj_remove_style_all(U.arc_glow);
    lv_obj_set_size(U.arc_glow, ring_d - ring_w + glow_w, ring_d - ring_w + glow_w);
    lv_obj_center(U.arc_glow);
    lv_arc_set_rotation(U.arc_glow, 0);
    lv_arc_set_bg_angles(U.arc_glow, 0, 0);
    lv_obj_set_style_arc_width(U.arc_glow, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(U.arc_glow, glow_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(U.arc_glow, 38, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(U.arc_glow, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(U.arc_glow, LV_OBJ_FLAG_CLICKABLE);

    U.arc = lv_arc_create(U.root);
    lv_obj_remove_style_all(U.arc);
    lv_obj_set_size(U.arc, ring_d, ring_d);
    lv_obj_center(U.arc);
    lv_arc_set_rotation(U.arc, 0);
    lv_arc_set_bg_angles(U.arc, (lv_value_precise_t)A0, (lv_value_precise_t)norm_deg(A0 + SWEEP));
    lv_obj_set_style_arc_width(U.arc, ring_w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(U.arc, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(U.arc, 20, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(U.arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(U.arc, ring_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(U.arc, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(U.arc, LV_OBJ_FLAG_CLICKABLE);

    /* the reading: a small dot in the state colour, ringed in the glass colour to lift it off the arc */
    U.now = kit_plain(U.root);
    lv_obj_set_size(U.now, (int32_t)lroundf(DS(6.5f)), (int32_t)lroundf(DS(6.5f)));
    lv_obj_set_style_radius(U.now, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.now, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(U.now, lv_color_hex(0x0b0e11), 0);
    lv_obj_set_style_border_width(U.now, (int32_t)lroundf(DS(1.25f)), 0);
    lv_obj_set_style_border_opa(U.now, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(U.now, (int32_t)lroundf(DS(4)), 0);
    lv_obj_set_style_shadow_opa(U.now, 150, 0);

    /* the target: a hollow white ring with a glow */
    U.target = kit_plain(U.root);
    lv_obj_set_size(U.target, (int32_t)lroundf(DS(9)), (int32_t)lroundf(DS(9)));
    lv_obj_set_style_radius(U.target, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(U.target, lv_color_hex(0x0b0e11), 0);
    lv_obj_set_style_bg_opa(U.target, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(U.target, lv_color_white(), 0);
    lv_obj_set_style_border_width(U.target, (int32_t)lroundf(DS(1.25f)), 0);
    lv_obj_set_style_border_opa(U.target, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(U.target, (int32_t)lroundf(DS(6)), 0);
    lv_obj_set_style_shadow_opa(U.target, 200, 0);

    /* the target's number, just inside the ring */
    U.tgt_lbl = lv_label_create(U.root);
    lv_obj_set_style_text_font(U.tgt_lbl, &brew_font_caps, 0);

    /* ---- home view ---- */
    U.v_home = make_view(U.root, DC(62), DC(10));
    /* status pill: "● COOLING" in spaced caps */
    U.pill = kit_plain(U.v_home);
    lv_obj_set_size(U.pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(U.pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(U.pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.pill, 36, 0);
    lv_obj_set_style_border_width(U.pill, 1, 0);
    lv_obj_set_style_border_opa(U.pill, 90, 0);
    lv_obj_set_style_pad_left(U.pill, (int32_t)lroundf(DS(8)), 0);
    lv_obj_set_style_pad_right(U.pill, (int32_t)lroundf(DS(9)), 0);
    lv_obj_set_style_pad_ver(U.pill, (int32_t)lroundf(DS(4.5f)), 0);
    lv_obj_set_style_pad_column(U.pill, (int32_t)lroundf(DS(4)), 0);
    U.st_dot = kit_plain(U.pill);
    lv_obj_set_size(U.st_dot, (int32_t)lroundf(DS(4)), (int32_t)lroundf(DS(4)));
    lv_obj_set_style_radius(U.st_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.st_dot, LV_OPA_COVER, 0);
    kit_breathe(U.st_dot);
    U.st_lbl = lv_label_create(U.pill);
    lv_obj_set_style_text_font(U.st_lbl, &brew_font_caps, 0);
    lv_obj_set_style_text_letter_space(U.st_lbl, (int32_t)lroundf(DS(1.6f)), 0);

    kit_nv_create(&U.big, U.v_home, &brew_font_num_big, &brew_font_deg_big, DC(22), DC(4));

    U.sub_lbl = lv_label_create(U.v_home);
    lv_obj_set_style_text_font(U.sub_lbl, &brew_font_small, 0);
    lv_obj_set_style_text_color(U.sub_lbl, lv_color_hex(0xa9b3bd), 0);

    /* ---- menu view ---- */
    U.v_menu = kit_plain(U.root);
    lv_obj_set_size(U.v_menu, DISP, DISP);
    lv_obj_set_style_opa(U.v_menu, LV_OPA_TRANSP, 0);
    kit_rows_create(&U.rows, U.v_menu, DC(44), DC(10), DC(5), false);
    U.list_title = lv_label_create(U.v_menu);
    lv_obj_set_style_text_font(U.list_title, &brew_font_caps, 0);
    lv_obj_set_style_text_letter_space(U.list_title, (int32_t)lroundf(DS(1.6f)), 0);
    lv_obj_set_style_text_color(U.list_title, lv_color_hex(0x66717c), 0);
    lv_obj_align(U.list_title, LV_ALIGN_TOP_MID, 0, DC(46));
    U.list_note = lv_label_create(U.v_menu);
    lv_obj_set_style_text_font(U.list_note, &brew_font_small, 0);
    lv_obj_set_style_text_color(U.list_note, lv_color_hex(0xa9b3bd), 0);
    lv_obj_set_style_text_align(U.list_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(U.list_note, DC(190));
    lv_label_set_long_mode(U.list_note, LV_LABEL_LONG_WRAP);
    lv_obj_align(U.list_note, LV_ALIGN_TOP_MID, 0, DC(64));

    /* ---- value editor ---- */
    kit_edit_create(&U.edit, U.root, 0, 0, DISP, DISP);

    /* ---- WiFi setup view ---- */
    kit_wifi_create(&U.wifi, U.root, 0, 0, DISP, DISP, (int32_t)lroundf(DS(80)));

    U.graph_t = lv_timer_create(graph_timer_cb, 50, NULL);    /* checks; renders per KIT_GRAPH_REDRAW_MS */

    U.wheel_pos = m->list_idx * 256;   /* a live layout swap can land mid-list */
    update_menu_items();
    layout_wheel();
    refresh(false);
}

/* ------------------------------------------------------------------ */
/* Model changes -> drawing                                           */
/* ------------------------------------------------------------------ */
static void round_update(const brew_model_t * m, uint32_t ch)
{
    M = m;
    const bool animate = (ch & BREW_CH_ANIMATE) != 0;

    if(ch & BREW_CH_VIEW) {             /* entering a view resets its presentation */
        if(M->view == BREW_VIEW_LIST) {   /* a new list (or back to one): rows refill, no scroll-in */
            update_menu_items();
            lv_anim_delete(&U.wheel_pos, wheel_exec);
            U.wheel_pos = M->list_idx * 256;
            layout_wheel();
        }
    }
    if(ch & (BREW_CH_GRAPH | BREW_CH_GRAPH_LAZY)) U.g.dirty = true;
    if(ch & BREW_CH_MENU_MOVE) {        /* the wheel animates on its own; nothing else moved */
        update_menu_items();
        wheel_animate_to(M->list_idx);
    }
    if(!(ch & BREW_CH_REFRESH)) return;  /* e.g. an appended sample: graph_t draws it */

    if((ch & BREW_CH_TEMP) && !(ch & BREW_CH_GRAPH)) {
        /* The live point moved: redraw only if that shows (>= 1 px or off scale), and leave it
         * to graph_t, which re-renders at most every KIT_GRAPH_REDRAW_MS. */
        if(kit_graph_live_moved(&U.g, M->temp_f)) U.g.dirty = true;
        const bool g = U.g.dirty;
        U.g.dirty = false;    /* don't re-render synchronously here */
        refresh(animate);
        U.g.dirty = g;
        return;
    }
    refresh(animate);
}

static void round_destroy(void)
{
    lv_anim_delete(&U.wheel_pos, wheel_exec);
    lv_timer_delete(U.graph_t);
    lv_obj_delete(U.root);   /* takes its children's animations with it */
    kit_graph_free(&U.g);
    memset(&U, 0, sizeof(U));
}

const brew_layout_t brew_layout_round = {
    .name = "round",
    .create = round_create,
    .update = round_update,
    .destroy = round_destroy,
};