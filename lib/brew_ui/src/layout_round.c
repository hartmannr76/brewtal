/**
 * layout_round.c - the round dial layout (LVGL 9.3): the original BrewDial look.
 *
 * Draws a brew_model_t (brew_model.h); owns no state of its own except presentation state
 * (objects, animations, the rendered graph). Selected through brew_layout_round.
 *
 * Layer stack (bottom -> top):
 *   bg[0], bg[1]   RGB565 canvases, dithered radial gradient; cross-faded on state change
 *   graph          ARGB8888 canvas with the history line, in the model's graph style (dimmable)
 *   halo, dot      "now"/scrub marker on the curve
 *   ticks          lv_scale, only visible while adjusting
 *   arc, knob      setpoint ring
 *   v_home / v_menu / v_hist   content views, cross-faded
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

    lv_obj_t * ticks;
    lv_obj_t * arc;
    lv_obj_t * knob;

    lv_obj_t * v_home;
    lv_obj_t * v_menu;
    lv_obj_t * v_hist;
    lv_obj_t * st_dot;
    lv_obj_t * st_lbl;
    lv_obj_t * sub_lbl;
    kit_numview_t big;
    kit_numview_t hval;
    lv_obj_t * htime;
    lv_obj_t * hhint;        /* "Turn to scrub · last 2 hours" */

    lv_obj_t * item[BREW_MENU_N];
    lv_obj_t * item_ico[BREW_MENU_N];
    lv_obj_t * item_txt[BREW_MENU_N];
    lv_obj_t * item_dot[BREW_MENU_N];
    int32_t wheel_pos; /* menu index * 256, animated */

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

static void update_ring(void)
{
    const float target = M->setting ? M->draft_f : M->sp_f[M->mode];
    const float a = frac_t(M->temp_f), b = frac_t(target);
    const float lo = a < b ? a : b, hi = a < b ? b : a;
    lv_arc_set_angles(U.arc, norm_deg(A0 + SWEEP * lo), norm_deg(A0 + SWEEP * hi + 0.6f));
    const lv_color_t ac = lv_color_hex(kit_line[brew_model_vstate(M)]);
    if(!kit_col_eq(lv_obj_get_style_arc_color(U.arc, LV_PART_INDICATOR), ac))
        lv_obj_set_style_arc_color(U.arc, ac, LV_PART_INDICATOR);
    const lv_opa_t ao = M->setting ? LV_OPA_COVER : 179;
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
    const bool home = M->view == BREW_VIEW_HOME;
    /* Fades re-render everything under the faded object every frame, so they're kept
     * short (BREW_FADE_MS). The ring/ticks cover nearly the whole screen. */
    kit_fade_to(U.v_home, home ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.v_menu, M->view == BREW_VIEW_MENU ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.v_hist, M->view == BREW_VIEW_HIST ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    /* hide fully transparent views so they cost nothing and can't be invalidated */
    kit_set_hidden(U.v_menu, M->view != BREW_VIEW_MENU && lv_obj_get_style_opa(U.v_menu, 0) == 0 && !lv_anim_get(U.v_menu, kit_opa_exec));
    kit_set_hidden(U.v_hist, M->view != BREW_VIEW_HIST && lv_obj_get_style_opa(U.v_hist, 0) == 0 && !lv_anim_get(U.v_hist, kit_opa_exec));

    kit_fade_to(U.arc, home ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.knob, home ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);
    kit_fade_to(U.ticks, home && M->setting ? LV_OPA_COVER : LV_OPA_TRANSP, BREW_FADE_MS);

    const lv_opa_t g = M->view == BREW_VIEW_MENU ? 64 : (M->setting ? 102 : LV_OPA_COVER);
    kit_fade_to(U.g.canvas, g, BREW_FADE_MS);
    kit_fade_to(U.g.halo, g, BREW_FADE_MS);
    kit_fade_to(U.g.dot, g, BREW_FADE_MS);
}

static void update_menu_items(void)
{
    static const char * icons[BREW_MENU_N] = { BREW_ICON_BEER, BREW_ICON_SNOWFLAKE, BREW_ICON_PAUSE, BREW_ICON_CHART, BREW_ICON_TEMP };
    char text[32];
    for(int i = 0; i < BREW_MENU_N; i++) {
        const char * ico = icons[i];
        if(i == BREW_MENU_PAUSE && M->paused) ico = BREW_ICON_PLAY;
        kit_set_text(U.item_ico[i], ico);
        brew_model_menu_label(M, i, text, sizeof(text));
        kit_set_text(U.item_txt[i], text);
        const lv_font_t * f = i == M->menu_idx ? &brew_font_menu_bold : &brew_font_menu;
        kit_set_font(U.item_ico[i], f);
        kit_set_font(U.item_txt[i], f);
        kit_set_hidden(U.item_dot[i], !brew_model_menu_active(M, i));
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
        kit_set_hidden(U.item[i], op <= 0.01f);
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
    const lv_color_t cool = lv_color_hex(KIT_COOL_TEXT);

    if(M->setting) {
        kit_set_text(U.st_lbl, "Target");
        kit_set_text_color(U.st_lbl, white);
        kit_set_text_opa(U.st_lbl, 179);
        kit_set_hidden(U.st_dot, true);
        kit_nv_set_color(&U.big, white);
        kit_set_text(U.sub_lbl, "Turn to adjust \xC2\xB7 press to set");
        brew_model_fmt_temp(M, t, sizeof(t), M->draft_f);
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
        kit_nv_set(&U.big, t, animate);
    }

    if(M->view == BREW_VIEW_HIST && brew_model_hist_last(M) >= 0) {
        brew_model_fmt_temp(M, t, sizeof(t), brew_model_hist_val(M, M->hist_idx));
        kit_nv_set(&U.hval, t, animate);
        kit_nv_set_color(&U.hval, vs == BREW_VS_COOL ? cool : white);
        brew_model_hist_time(M, M->hist_idx, s, sizeof(s));
        kit_set_text(U.htime, s);
        char w[32];
        brew_model_hist_window_text(M, w, sizeof(w));
        snprintf(s, sizeof(s), "Turn to scrub \xC2\xB7 %s", w);
        kit_set_text(U.hhint, s);
    }

    if(M->view == BREW_VIEW_MENU) update_menu_items();
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

    U.knob = kit_plain(U.root);
    lv_obj_set_size(U.knob, (int32_t)lroundf(DS(10)), (int32_t)lroundf(DS(10)));
    lv_obj_set_style_radius(U.knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(U.knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(U.knob, LV_OPA_COVER, 0);

    /* ---- home view ---- */
    U.v_home = make_view(U.root, DC(62), DC(10));
    lv_obj_t * st_row = kit_plain(U.v_home);
    lv_obj_set_size(st_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(st_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st_row, DC(7), 0);
    U.st_dot = kit_plain(st_row);
    lv_obj_set_size(U.st_dot, DC(6), DC(6));
    lv_obj_set_style_radius(U.st_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.st_dot, LV_OPA_COVER, 0);
    kit_breathe(U.st_dot);
    U.st_lbl = lv_label_create(st_row);
    lv_obj_set_style_text_font(U.st_lbl, &brew_font_status, 0);

    kit_nv_create(&U.big, U.v_home, &brew_font_num_big, &brew_font_deg_big, DC(17), DC(2));

    U.sub_lbl = lv_label_create(U.v_home);
    lv_obj_set_style_text_font(U.sub_lbl, &brew_font_small, 0);
    lv_obj_set_style_text_color(U.sub_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_opa(U.sub_lbl, 140, 0);

    /* ---- menu view ---- */
    U.v_menu = kit_plain(U.root);
    lv_obj_set_size(U.v_menu, DISP, DISP);
    lv_obj_set_style_opa(U.v_menu, LV_OPA_TRANSP, 0);
    for(int i = 0; i < 5; i++) {
        lv_obj_t * it = kit_plain(U.v_menu);
        lv_obj_set_size(it, LV_SIZE_CONTENT, DC(44));
        lv_obj_set_flex_flow(it, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(it, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(it, DC(10), 0);
        U.item_ico[i] = lv_label_create(it);
        lv_obj_set_style_text_color(U.item_ico[i], lv_color_white(), 0);
        lv_obj_set_style_text_opa(U.item_ico[i], 230, 0);
        U.item_txt[i] = lv_label_create(it);
        lv_obj_set_style_text_color(U.item_txt[i], lv_color_white(), 0);
        U.item_dot[i] = kit_plain(it);
        lv_obj_set_size(U.item_dot[i], DC(5), DC(5));
        lv_obj_set_style_radius(U.item_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(U.item_dot[i], lv_color_hex(KIT_COOL_TEXT), 0);
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
    kit_nv_create(&U.hval, U.v_hist, &brew_font_num_hist, &brew_font_deg_hist, DC(12), DC(2));
    U.hhint = lv_label_create(U.v_hist);   /* text set in refresh(): the window comes from the model */
    lv_obj_set_style_text_font(U.hhint, &brew_font_small, 0);
    lv_obj_set_style_text_color(U.hhint, lv_color_white(), 0);
    lv_obj_set_style_text_opa(U.hhint, 140, 0);

    U.graph_t = lv_timer_create(graph_timer_cb, 50, NULL);    /* checks; renders per KIT_GRAPH_REDRAW_MS */

    U.wheel_pos = m->menu_idx * 256;   /* a live layout swap can land mid-menu */
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
        if(M->view == BREW_VIEW_MENU) {
            lv_anim_delete(&U.wheel_pos, wheel_exec);
            U.wheel_pos = M->menu_idx * 256;
            layout_wheel();
        }
        if(M->view == BREW_VIEW_HIST) U.hval.cur[0] = 0;   /* no slide-in on entry */
    }
    if(ch & (BREW_CH_GRAPH | BREW_CH_GRAPH_LAZY)) U.g.dirty = true;
    if(ch & BREW_CH_MENU_MOVE) {        /* the wheel animates on its own; nothing else moved */
        update_menu_items();
        wheel_animate_to(M->menu_idx);
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