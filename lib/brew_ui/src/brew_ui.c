/**
 * brew_ui.c - the public brew_ui API: owns the model (brew_model.c) and the active layout
 * (brew_layout.h), forwards every call to the model and every change to the layout, and runs
 * the auto-commit timer.
 */
#include "brew_ui.h"
#include "brew_layout.h"
#include "brew_model.h"

static struct {
    brew_model_t m;
    const brew_layout_t * layout;
    lv_timer_t * commit_t;
} S;

static void apply(uint32_t ch)
{
    if(ch & BREW_CH_ARM_COMMIT) {
        lv_timer_reset(S.commit_t);
        lv_timer_resume(S.commit_t);
    }
    if(ch & BREW_CH_DISARM_COMMIT) lv_timer_pause(S.commit_t);
    if(ch && S.layout) S.layout->update(&S.m, ch);
}

static void commit_timer_cb(lv_timer_t * t)
{
    LV_UNUSED(t);
    apply(brew_model_commit(&S.m));
}

void brew_ui_create_with_layout(lv_obj_t * screen, const brew_ui_cbs_t * cbs, const brew_layout_t * layout)
{
    brew_model_init(&S.m, cbs);
    S.layout = layout ? layout : &brew_layout_round;
    S.commit_t = lv_timer_create(commit_timer_cb, BREW_COMMIT_MS, NULL);
    lv_timer_pause(S.commit_t);
    S.layout->create(screen, &S.m);
}

void brew_ui_create(lv_obj_t * screen, const brew_ui_cbs_t * cbs)
{
    brew_ui_create_with_layout(screen, cbs, &brew_layout_round);
}

/* ---- controller -> UI ---- */
void brew_ui_set_temp(float f) { apply(brew_model_set_temp(&S.m, f)); }
void brew_ui_set_action(brew_action_t a) { apply(brew_model_set_action(&S.m, a)); }
void brew_ui_set_setpoints(float ferment_f, float crash_f) { apply(brew_model_set_setpoints(&S.m, ferment_f, crash_f)); }
void brew_ui_set_mode(brew_mode_t m, bool paused) { apply(brew_model_set_mode(&S.m, m, paused)); }
void brew_ui_set_units(bool celsius) { apply(brew_model_set_units(&S.m, celsius)); }
void brew_ui_set_batch(const char * name, int day) { apply(brew_model_set_batch(&S.m, name, day)); }
void brew_ui_set_history(const float * t, int n, int minutes_per_sample)
{
    apply(brew_model_set_history(&S.m, t, n, minutes_per_sample));
}
void brew_ui_push_history(float f) { apply(brew_model_push_history(&S.m, f)); }

/* ---- input -> UI ---- */
void brew_ui_encoder(int detents) { apply(brew_model_encoder(&S.m, detents)); }
void brew_ui_press(void) { apply(brew_model_press(&S.m)); }
