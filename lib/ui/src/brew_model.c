/**
 * brew_model.c - the UI's state and rules (see brew_model.h). No LVGL here: this file is
 * compiled into the host unit tests on its own.
 */
#include "brew_model.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint32_t set_view(brew_model_t * m, brew_view_t v);

void brew_model_init(brew_model_t * m, const brew_ui_cbs_t * cbs)
{
    memset(m, 0, sizeof(*m));
    if(cbs) m->cbs = *cbs;
    m->hist = (float *)calloc(BREW_HIST_MAX, sizeof(float));   /* large: malloc puts it in PSRAM on the S3 */
    m->temp_f = 66.0f;
    m->sp_f[0] = 66.0f;
    m->sp_f[1] = 34.0f;
    m->hist_ms_per = 200;
    m->view = BREW_VIEW_HOME;
    m->heat_diff_f = 0.5f;
    m->cool_diff_f = 0.5f;
    m->heat_delay_s = 180;
    m->cool_delay_s = 180;
    m->crash_default_f = 34.0f;
    m->sched.nsteps = 0;
    m->bf_last_log_min = -1;
    m->bf_today_day = -1;
    m->bf_today_step = -1;   /* empty until the user builds one */
}

void brew_model_free(brew_model_t * m)
{
    free(m->hist);
    m->hist = NULL;
    m->hist_n = 0;
}

/* ------------------------------------------------------------------ derived */

brew_vstate_t brew_model_vstate(const brew_model_t * m)
{
    if(m->view == BREW_VIEW_WIFI) {   /* the WiFi screens light the glass by how setup is going */
        const brew_wifi_screen_t s = brew_model_wifi_screen(m);
        return s == BREW_WIFI_SCREEN_DONE ? BREW_VS_OK : s == BREW_WIFI_SCREEN_FAILED ? BREW_VS_HEAT : BREW_VS_COOL;
    }
    if(m->paused) return BREW_VS_PAUSE;
    if(m->mode == BREW_MODE_CRASH) return BREW_VS_CRASH;
    if(m->action == BREW_ACTION_COOLING) return BREW_VS_COOL;
    if(m->action == BREW_ACTION_HEATING) return BREW_VS_HEAT;
    return BREW_VS_IDLE;
}

float brew_model_shown_target(const brew_model_t * m)
{
    return m->setting ? m->draft_f : m->sp_f[m->mode];
}

float brew_model_to_disp(const brew_model_t * m, float f)
{
    return m->celsius ? (f - 32.0f) * 5.0f / 9.0f : f;
}

void brew_model_fmt_temp(const brew_model_t * m, char * buf, size_t n, float f)
{
    snprintf(buf, n, "%.1f", (double)brew_model_to_disp(m, f));
}

bool brew_model_waiting(const brew_model_t * m)
{
    return !m->paused && m->action == BREW_ACTION_IDLE && m->wanted != BREW_ACTION_IDLE;
}

static void status_state_text(const brew_model_t * m, char * buf, size_t n);

void brew_model_status_text(const brew_model_t * m, char * buf, size_t n)
{
    if(!m->demo) {
        status_state_text(m, buf, n);
        return;
    }
    char s[48];
    status_state_text(m, s, sizeof(s));
    snprintf(buf, n, "Demo \xC2\xB7 %s", s);
}

static void status_state_text(const brew_model_t * m, char * buf, size_t n)
{
    if(!m->temp_known && !m->paused) {
        snprintf(buf, n, "No temperature reading");
        return;
    }
    const int tgt = (int)lroundf(brew_model_to_disp(m, m->sp_f[m->mode]));
    const brew_vstate_t vs = brew_model_vstate(m);
    if(brew_model_waiting(m) && (vs == BREW_VS_IDLE || vs == BREW_VS_CRASH)) {
        const char * what = m->wanted == BREW_ACTION_HEATING ? "Heating"
                            : vs == BREW_VS_CRASH            ? "Crashing"
                                                             : "Cooling";
        snprintf(buf, n, "%s in %d min", what, m->wait_min);
        return;
    }
    switch(vs) {
        case BREW_VS_COOL:  snprintf(buf, n, "Cooling to %d\xC2\xB0", tgt); break;
        case BREW_VS_IDLE:  snprintf(buf, n, "Holding at %d\xC2\xB0", tgt); break;
        case BREW_VS_HEAT:  snprintf(buf, n, "Heating to %d\xC2\xB0", tgt); break;
        case BREW_VS_CRASH: snprintf(buf, n, "Crashing to %d\xC2\xB0", tgt); break;
        default:            snprintf(buf, n, "Paused"); break;
    }
}

void brew_model_batch_text(const brew_model_t * m, char * buf, size_t n)
{
    const char * sep = m->batch[0] ? " \xC2\xB7 " : "";
    if(m->mode == BREW_MODE_CRASH) snprintf(buf, n, "%s%sCold crash", m->batch, sep);
    else snprintf(buf, n, "%s%sDay %d", m->batch, sep, m->day);
}

static void pill_state_text(const brew_model_t * m, char * buf, size_t n);

/* The state pill. A demo build says so first, on every screen shape. */
void brew_model_pill_text(const brew_model_t * m, char * buf, size_t n)
{
    if(!m->demo) {
        pill_state_text(m, buf, n);
        return;
    }
    char s[40];
    pill_state_text(m, s, sizeof(s));
    snprintf(buf, n, "DEMO \xC2\xB7 %s", s);
}

static void pill_state_text(const brew_model_t * m, char * buf, size_t n)
{
    if(m->setting) {
        snprintf(buf, n, "SET TARGET");
        return;
    }
    if(!m->temp_known && !m->paused) {   /* no probe, or it stopped answering: outputs are off */
        snprintf(buf, n, "NO READING");
        return;
    }
    const brew_vstate_t vs = brew_model_vstate(m);
    if(brew_model_waiting(m) && (vs == BREW_VS_IDLE || vs == BREW_VS_CRASH)) {
        const char * what = m->wanted == BREW_ACTION_HEATING ? "HEATING"
                            : vs == BREW_VS_CRASH            ? "CRASHING"
                                                             : "COOLING";
        snprintf(buf, n, "%s IN %d MIN", what, m->wait_min);
        return;
    }
    switch(vs) {
        case BREW_VS_COOL:  snprintf(buf, n, "COOLING"); break;
        case BREW_VS_IDLE:  snprintf(buf, n, "HOLDING"); break;
        case BREW_VS_HEAT:  snprintf(buf, n, "HEATING"); break;
        case BREW_VS_CRASH: snprintf(buf, n, "CRASHING"); break;
        default:            snprintf(buf, n, "PAUSED"); break;
    }
}

/* "4 d", "1 d", "12 h", "1 d 12 h" */
static void fmt_dur(char * buf, size_t n, int h)
{
    if(h % 24 == 0) snprintf(buf, n, "%d d", h / 24);
    else if(h < 24) snprintf(buf, n, "%d h", h);
    else snprintf(buf, n, "%d d %d h", h / 24, h % 24);
}

static bool schedule_shown(const brew_model_t * m)
{
    return m->sched.profile == BREW_PROFILE_SCHEDULE && m->sched.nsteps > 0 && m->mode == BREW_MODE_FERMENT;
}

void brew_model_detail_text(const brew_model_t * m, char * buf, size_t n)
{
    if(m->setting) {
        snprintf(buf, n, "Turn to adjust \xC2\xB7 press to set");
        return;
    }
    if(schedule_shown(m)) {
        const brew_schedule_t * s = &m->sched;
        if(s->step == BREW_STEP_DONE || s->step >= s->nsteps) {
            snprintf(buf, n, "%s", s->override_on ? "Schedule done \xC2\xB7 your target" : "Schedule done");
            return;
        }
        if(s->override_on) {
            if(s->step + 1 < s->nsteps) snprintf(buf, n, "Step %d/%d \xC2\xB7 your target until step %d", s->step + 1, s->nsteps, s->step + 2);
            else snprintf(buf, n, "Step %d/%d \xC2\xB7 your target", s->step + 1, s->nsteps);
            return;
        }
        const brew_step_t * st = &s->steps[s->step];
        char r[16], h[16];
        fmt_dur(h, sizeof(h), st->hold_h);
        if(st->ramp_h > 0) {
            fmt_dur(r, sizeof(r), st->ramp_h);
            snprintf(buf, n, "Step %d/%d \xC2\xB7 ramp %s \xC2\xB7 hold %s", s->step + 1, s->nsteps, r, h);
        }
        else {
            snprintf(buf, n, "Step %d/%d \xC2\xB7 hold %s", s->step + 1, s->nsteps, h);
        }
        return;
    }
    char t[16], b[48];
    brew_model_fmt_temp(m, t, sizeof(t), m->sp_f[m->mode]);
    brew_model_batch_text(m, b, sizeof(b));
    snprintf(buf, n, "Target %s\xC2\xB0 \xC2\xB7 %s", t, b);
}

const char * brew_model_unit(const brew_model_t * m) { return m->celsius ? "\xC2\xB0" "C" : "\xC2\xB0" "F"; }

int brew_model_hist_last(const brew_model_t * m) { return m->temp_known ? m->hist_n : m->hist_n - 1; }

float brew_model_hist_val(const brew_model_t * m, int i) { return i < m->hist_n ? m->hist[i] : m->temp_f; }

/* ------------------------------------------------------------------ lists */

enum {
    ACT_NONE, ACT_FERMENT, ACT_CRASH, ACT_PAUSE, ACT_OPEN, ACT_BACK, ACT_UNITS, ACT_WIFI_SETUP, ACT_WIFI_FORGET,
    ACT_EDIT, ACT_CONSTANT, ACT_RESET_DAY, ACT_STEP, ACT_ADD_STEP, ACT_USE_SCHEDULE, ACT_STOP_SCHEDULE,
    ACT_REMOVE_STEP, ACT_BF_CHECK, ACT_BF_PICK, ACT_BF_RUN, ACT_BF_RUN_OK,
};

static void fmt_temp_unit(const brew_model_t * m, char * buf, size_t n, float f)
{
    snprintf(buf, n, "%.1f%s", (double)brew_model_to_disp(m, f), brew_model_unit(m));
}

static void fmt_diff_unit(const brew_model_t * m, char * buf, size_t n, float d_f)
{
    snprintf(buf, n, "%.1f%s", (double)(m->celsius ? d_f * 5.0f / 9.0f : d_f), brew_model_unit(m));
}

static const char * bf_account_short(brew_bf_account_t a)
{
    switch(a) {
        case BREW_BF_CHECKING:    return "checking";
        case BREW_BF_CONNECTED:   return "connected";
        case BREW_BF_REJECTED:    return "key refused";
        case BREW_BF_UNREACHABLE: return "offline";
        default:                  return "not set up";
    }
}

static brew_list_item_t * add(brew_list_item_t * it, int * n, const char * label, brew_icon_t icon, uint8_t act, uint8_t arg)
{
    brew_list_item_t * x = &it[(*n)++];
    memset(x, 0, sizeof(*x));
    snprintf(x->label, sizeof(x->label), "%s", label);
    x->icon = icon;
    x->act = act;
    x->arg = arg;
    x->opens = act == ACT_OPEN || act == ACT_EDIT;
    x->back = act == ACT_BACK;
    return x;
}

/* Every list, built on demand (small, and always current). Previous is always the last row. */
static int build(const brew_model_t * m, brew_list_t l, brew_list_item_t * it)
{
    int n = 0;
    brew_list_item_t * x;
    switch(l) {
        case BREW_LIST_MENU:
            x = add(it, &n, "Ferment", BREW_IC_BEER, ACT_FERMENT, 0);
            x->mark = !m->paused && m->mode == BREW_MODE_FERMENT ? BREW_MARK_ACTIVE : BREW_MARK_NONE;
            x = add(it, &n, "Cold crash", BREW_IC_SNOWFLAKE, ACT_CRASH, 0);
            x->mark = !m->paused && m->mode == BREW_MODE_CRASH ? BREW_MARK_ACTIVE : BREW_MARK_NONE;
            add(it, &n, m->paused ? "Resume" : "Pause", m->paused ? BREW_IC_PLAY : BREW_IC_PAUSE, ACT_PAUSE, 0);
            add(it, &n, "Settings", BREW_IC_SETTINGS, ACT_OPEN, BREW_LIST_SETTINGS);
            break;
        case BREW_LIST_SETTINGS:
            x = add(it, &n, "Units", BREW_IC_TEMP, ACT_UNITS, 0);
            snprintf(x->value, sizeof(x->value), "%s", brew_model_unit(m));
            x = add(it, &n, "WiFi", BREW_IC_WIFI, ACT_OPEN, BREW_LIST_WIFI);
            if(m->wifi_phase == BREW_WIFI_CONNECTED) snprintf(x->value, sizeof(x->value), "%.14s%s", m->wifi_ssid, strlen(m->wifi_ssid) > 14 ? "..." : "");
            else snprintf(x->value, sizeof(x->value), "%s", m->wifi_phase == BREW_WIFI_NONE ? "none" : m->wifi_phase == BREW_WIFI_OFFLINE ? "offline" : "off");
            add(it, &n, "Heat", BREW_IC_FLAME, ACT_OPEN, BREW_LIST_HEAT);
            add(it, &n, "Cool", BREW_IC_SNOWFLAKE, ACT_OPEN, BREW_LIST_COOL);
            x = add(it, &n, "Profile", BREW_IC_PROFILE, ACT_OPEN, BREW_LIST_PROFILE);
            snprintf(x->value, sizeof(x->value), "%s", m->sched.profile == BREW_PROFILE_SCHEDULE ? "Schedule" : "Constant");
            x = add(it, &n, "Brewfather", BREW_IC_BEER, ACT_OPEN, BREW_LIST_BF_ACCOUNT);
            snprintf(x->value, sizeof(x->value), "%s", bf_account_short(m->bf_account));
            break;
        case BREW_LIST_BREWFATHER:
            if(m->bf_account == BREW_BF_NOT_SET_UP) break;
            for(int i = 0; i < m->bf_nbatches; i++) add(it, &n, m->bf_batches[i], BREW_IC_BEER, ACT_BF_PICK, (uint8_t)i);
            add(it, &n, "Refresh", BREW_IC_NONE, ACT_BF_CHECK, 0);
            break;
        case BREW_LIST_BF_PREVIEW:
            if(m->bf_preview != BREW_BF_PREVIEW_READY) break;
            for(int i = 0; i < m->bf_preview_n; i++) {   /* as the Schedule list shows steps: "2 · 53.0°F  ramp 1 d, 14 d" */
                const brew_step_t * st = &m->bf_preview_steps[i];
                char lbl[32], h[16], r[16];
                snprintf(lbl, sizeof(lbl), "%d \xC2\xB7 ", i + 1);
                fmt_temp_unit(m, lbl + strlen(lbl), sizeof(lbl) - strlen(lbl), st->temp_f);
                x = add(it, &n, lbl, BREW_IC_NONE, ACT_NONE, 0);
                x->mark = i == m->bf_today_step ? BREW_MARK_ACTIVE : BREW_MARK_NONE;
                fmt_dur(h, sizeof(h), st->hold_h);
                if(st->ramp_h > 0) {
                    fmt_dur(r, sizeof(r), st->ramp_h);
                    snprintf(x->value, sizeof(x->value), "ramp %s, %s", r, h);
                }
                else {
                    snprintf(x->value, sizeof(x->value), "%s", h);
                }
            }
            add(it, &n, "Run this profile", BREW_IC_PLAY, ACT_BF_RUN, 0);
            break;
        case BREW_LIST_BF_NOTICE:
            add(it, &n, "OK", BREW_IC_NONE, ACT_BF_RUN_OK, 0);
            return n;   /* no Previous: OK is the only way on */
        case BREW_LIST_BF_ACCOUNT:
            x = add(it, &n, "Account", BREW_IC_NONE, ACT_NONE, 0);
            snprintf(x->value, sizeof(x->value), "%s", bf_account_short(m->bf_account));
            x = add(it, &n, "Logging", BREW_IC_NONE, ACT_NONE, 0);
            if(!m->bf_logging) snprintf(x->value, sizeof(x->value), "off");
            else if(m->bf_attached) snprintf(x->value, sizeof(x->value), "%.18s%s", m->bf_batch, strlen(m->bf_batch) > 18 ? "..." : "");
            else if(m->bf_account == BREW_BF_CONNECTED) snprintf(x->value, sizeof(x->value), "not attached");
            else snprintf(x->value, sizeof(x->value), "on");
            if(m->bf_last_log_min >= 0) {
                x = add(it, &n, "Last reading", BREW_IC_NONE, ACT_NONE, 0);
                const int mi = m->bf_last_log_min;
                if(mi < 2) snprintf(x->value, sizeof(x->value), "just now");
                else if(mi < 90) snprintf(x->value, sizeof(x->value), "%d min ago", mi);
                else if(mi < 48 * 60) snprintf(x->value, sizeof(x->value), "%d h ago", (mi + 30) / 60);
                else snprintf(x->value, sizeof(x->value), "%d days ago", (mi + 720) / 1440);
            }
            if(m->bf_account != BREW_BF_NOT_SET_UP) add(it, &n, "Check now", BREW_IC_NONE, ACT_BF_CHECK, 0);
            break;
        case BREW_LIST_WIFI:
            if(m->wifi_phase == BREW_WIFI_CONNECTED || m->wifi_phase == BREW_WIFI_OFFLINE) {
                add(it, &n, "Change network", BREW_IC_WIFI, ACT_WIFI_SETUP, 0);
                add(it, &n, "Forget network", BREW_IC_NONE, ACT_WIFI_FORGET, 0);
            }
            else if(m->wifi_phase == BREW_WIFI_OFF) {
                add(it, &n, "Set up WiFi", BREW_IC_WIFI, ACT_WIFI_SETUP, 0);
            }
            break;
        case BREW_LIST_HEAT:
            x = add(it, &n, "Differential", BREW_IC_NONE, ACT_EDIT, BREW_EDIT_HEAT_DIFF);
            fmt_diff_unit(m, x->value, sizeof(x->value), m->heat_diff_f);
            x = add(it, &n, "Restart delay", BREW_IC_NONE, ACT_EDIT, BREW_EDIT_HEAT_DELAY);
            snprintf(x->value, sizeof(x->value), "%d min", (m->heat_delay_s + 30) / 60);
            break;
        case BREW_LIST_COOL:
            x = add(it, &n, "Differential", BREW_IC_NONE, ACT_EDIT, BREW_EDIT_COOL_DIFF);
            fmt_diff_unit(m, x->value, sizeof(x->value), m->cool_diff_f);
            x = add(it, &n, "Restart delay", BREW_IC_NONE, ACT_EDIT, BREW_EDIT_COOL_DELAY);
            snprintf(x->value, sizeof(x->value), "%d min", (m->cool_delay_s + 30) / 60);
            x = add(it, &n, "Cold crash", BREW_IC_NONE, ACT_EDIT, BREW_EDIT_CRASH);
            fmt_temp_unit(m, x->value, sizeof(x->value), m->crash_default_f);
            break;
        case BREW_LIST_PROFILE:
            x = add(it, &n, "Constant", BREW_IC_NONE, ACT_CONSTANT, 0);
            x->mark = m->sched.profile == BREW_PROFILE_CONSTANT ? BREW_MARK_CHOSEN : BREW_MARK_NONE;
            x = add(it, &n, "Schedule", BREW_IC_NONE, ACT_OPEN, BREW_LIST_SCHEDULE);
            x->mark = m->sched.profile == BREW_PROFILE_SCHEDULE ? BREW_MARK_CHOSEN : BREW_MARK_NONE;
            x = add(it, &n, "Brewfather", BREW_IC_NONE, ACT_OPEN, BREW_LIST_BREWFATHER);
            if(m->bf_account == BREW_BF_CONNECTED && m->bf_nbatches > 0)
                snprintf(x->value, sizeof(x->value), "%d batch%s", m->bf_nbatches, m->bf_nbatches == 1 ? "" : "es");
            x = add(it, &n, "Restart day count", BREW_IC_CALENDAR, ACT_RESET_DAY, 0);
            snprintf(x->value, sizeof(x->value), "Day %d", m->day);
            break;
        case BREW_LIST_SCHEDULE: {
            const brew_schedule_t * s = &m->sched;
            for(int i = 0; i < s->nsteps && i < BREW_STEPS_MAX; i++) {
                char l[32], d[16], r[16];
                snprintf(l, sizeof(l), "%d \xC2\xB7 ", i + 1);
                fmt_temp_unit(m, l + strlen(l), sizeof(l) - strlen(l), s->steps[i].temp_f);
                x = add(it, &n, l, BREW_IC_NONE, ACT_STEP, (uint8_t)i);
                fmt_dur(d, sizeof(d), s->steps[i].hold_h);
                if(s->steps[i].ramp_h > 0) {
                    fmt_dur(r, sizeof(r), s->steps[i].ramp_h);
                    snprintf(x->value, sizeof(x->value), "ramp %s, %s", r, d);
                }
                else {
                    snprintf(x->value, sizeof(x->value), "%s", d);
                }
                x->opens = true;
                if(s->profile == BREW_PROFILE_SCHEDULE && s->step == i) x->mark = BREW_MARK_ACTIVE;
            }
            if(s->nsteps < BREW_STEPS_MAX) add(it, &n, "Add step", BREW_IC_NONE, ACT_ADD_STEP, 0);
            if(s->profile == BREW_PROFILE_SCHEDULE) add(it, &n, "Stop schedule", BREW_IC_PAUSE, ACT_STOP_SCHEDULE, 0);
            else if(s->nsteps > 0) add(it, &n, "Use this schedule", BREW_IC_PLAY, ACT_USE_SCHEDULE, 0);
            break;
        }
        case BREW_LIST_STEP: {
            const brew_step_t * st = &m->sched.steps[m->step_sel];
            x = add(it, &n, "Temperature", BREW_IC_NONE, ACT_EDIT, BREW_EDIT_STEP_TEMP);
            fmt_temp_unit(m, x->value, sizeof(x->value), st->temp_f);
            x = add(it, &n, "Ramp", BREW_IC_NONE, ACT_EDIT, BREW_EDIT_STEP_RAMP);
            if(st->ramp_h > 0) fmt_dur(x->value, sizeof(x->value), st->ramp_h);
            else snprintf(x->value, sizeof(x->value), "jump");
            x = add(it, &n, "Hold", BREW_IC_NONE, ACT_EDIT, BREW_EDIT_STEP_HOLD);
            fmt_dur(x->value, sizeof(x->value), st->hold_h);
            add(it, &n, "Remove step", BREW_IC_NONE, ACT_REMOVE_STEP, 0);
            break;
        }
        case BREW_LIST_N:
            break;
    }
    add(it, &n, "Previous", BREW_IC_BACK, ACT_BACK, 0);
    return n;
}

/* Where Previous goes: the parent list, on the row that opened this one (the menu: home). */
static void parent(const brew_model_t * m, brew_list_t l, brew_list_t * pl, int * pi)
{
    switch(l) {
        case BREW_LIST_SCHEDULE:   *pl = BREW_LIST_PROFILE; *pi = 1; break;
        case BREW_LIST_STEP:       *pl = BREW_LIST_SCHEDULE; *pi = m->step_sel; break;
        case BREW_LIST_SETTINGS:   *pl = BREW_LIST_MENU; *pi = 3; break;
        case BREW_LIST_WIFI:       *pl = BREW_LIST_SETTINGS; *pi = 1; break;
        case BREW_LIST_HEAT:       *pl = BREW_LIST_SETTINGS; *pi = 2; break;
        case BREW_LIST_COOL:       *pl = BREW_LIST_SETTINGS; *pi = 3; break;
        case BREW_LIST_PROFILE:    *pl = BREW_LIST_SETTINGS; *pi = 4; break;
        case BREW_LIST_BREWFATHER: *pl = BREW_LIST_PROFILE; *pi = 2; break;
        case BREW_LIST_BF_ACCOUNT: *pl = BREW_LIST_SETTINGS; *pi = 5; break;
        case BREW_LIST_BF_PREVIEW: *pl = BREW_LIST_BREWFATHER; *pi = m->bf_pick; break;
        case BREW_LIST_BF_NOTICE:  *pl = BREW_LIST_BF_PREVIEW; *pi = 0; break;
        default:                   *pl = BREW_LIST_N; *pi = 0; break;   /* home */
    }
}

int brew_model_list_count(const brew_model_t * m)
{
    brew_list_item_t it[BREW_LIST_MAX];
    return build(m, m->list, it);
}

void brew_model_list_item(const brew_model_t * m, int i, brew_list_item_t * out)
{
    brew_list_item_t it[BREW_LIST_MAX];
    const int n = build(m, m->list, it);
    if(i < 0 || i >= n) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = it[i];
}

const char * brew_model_list_title(const brew_model_t * m)
{
    static const char * const t[BREW_LIST_N] = { "", "SETTINGS", "WIFI", "HEAT", "COOL", "PROFILE", "BREWFATHER", "SCHEDULE", "",
                                                 "BREWFATHER", "IMPORT", "" };
    static char step[16];   /* the UI runs in one task */
    if(m->list == BREW_LIST_STEP) {
        snprintf(step, sizeof(step), "STEP %d", m->step_sel + 1);
        return step;
    }
    return m->list < BREW_LIST_N ? t[m->list] : "";
}

void brew_model_list_note(const brew_model_t * m, char * buf, size_t n)
{
    if(n) buf[0] = 0;
    switch(m->list) {
        case BREW_LIST_WIFI:
            switch(m->wifi_phase) {
                case BREW_WIFI_NONE:      snprintf(buf, n, "This display has no WiFi"); break;
                case BREW_WIFI_OFF:       snprintf(buf, n, "Not set up"); break;
                case BREW_WIFI_CONNECTED: snprintf(buf, n, "On %s", m->wifi_ssid); break;
                case BREW_WIFI_OFFLINE:   snprintf(buf, n, "Not connected: retrying %s", m->wifi_ssid); break;
                default:                  snprintf(buf, n, "Setting up"); break;
            }
            break;
        case BREW_LIST_BREWFATHER:
            if(m->bf_account == BREW_BF_NOT_SET_UP) snprintf(buf, n, "Set up in Settings \xC2\xBB Brewfather");
            else if(m->bf_nbatches > 0) snprintf(buf, n, "Fermenting in Brewfather");
            else if(m->bf_account == BREW_BF_CONNECTED) snprintf(buf, n, "No Fermenting batches in Brewfather");
            else if(m->bf_account == BREW_BF_CHECKING) snprintf(buf, n, "Checking Brewfather...");
            else snprintf(buf, n, "Can't reach Brewfather");
            break;
        case BREW_LIST_BF_PREVIEW:
            if(m->bf_preview == BREW_BF_PREVIEW_LOADING) snprintf(buf, n, "Loading %s...", m->bf_preview_label);
            else if(m->bf_preview != BREW_BF_PREVIEW_READY) snprintf(buf, n, "Couldn't load this batch");
            else if(m->bf_today_day < 1) snprintf(buf, n, "%s \xC2\xB7 starts when you run it", m->bf_preview_label);
            else if(m->bf_today_step >= 0) snprintf(buf, n, "Day %d today \xC2\xB7 step %d of %d", m->bf_today_day, m->bf_today_step + 1, m->bf_preview_n);
            else snprintf(buf, n, "Day %d today \xC2\xB7 past the last step", m->bf_today_day);
            break;
        case BREW_LIST_BF_NOTICE: snprintf(buf, n, "Only the first %d steps are imported", BREW_STEPS_MAX); break;
        case BREW_LIST_BF_ACCOUNT:
            if(m->wifi_phase != BREW_WIFI_CONNECTED || !m->wifi_address[0]) snprintf(buf, n, "Needs WiFi");
            else snprintf(buf, n, "Set up on your phone:\n%s/brewfather", m->wifi_address);
            break;
        case BREW_LIST_SCHEDULE: {
            const brew_schedule_t * s = &m->sched;
            int h = 0;
            for(int i = 0; i < s->nsteps; i++) h += s->steps[i].ramp_h + s->steps[i].hold_h;
            char d[16];
            fmt_dur(d, sizeof(d), h);
            if(s->nsteps == 0) snprintf(buf, n, "No steps yet");
            else if(s->profile != BREW_PROFILE_SCHEDULE) snprintf(buf, n, "%d %s \xC2\xB7 %s", s->nsteps, s->nsteps == 1 ? "step" : "steps", d);
            else if(s->step == BREW_STEP_DONE || s->step >= s->nsteps) snprintf(buf, n, "Done: holding the last step");
            else snprintf(buf, n, "Running \xC2\xB7 step %d of %d", s->step + 1, s->nsteps);
            break;
        }
        default: break;
    }
}

/* ------------------------------------------------------------------ the value editor */

typedef struct {
    const char * title;
    float min_f, max_f, step_f;   /* in F (temperatures, differences) or minutes */
    float min_c, max_c, step_c;   /* the same in C */
    brew_list_t list;
    int row;
    brew_tuning_t tune;
} edit_def_t;

static const edit_def_t s_edit[BREW_EDIT_N] = {
    { "HEAT \xC2\xB7 DIFFERENTIAL",  0.1f, 5.0f, 0.1f,   0.1f, 3.0f, 0.1f,  BREW_LIST_HEAT, 0, BREW_TUNE_HEAT_DIFF },
    { "HEAT \xC2\xB7 RESTART DELAY", 0.0f, 30.0f, 1.0f,  0.0f, 30.0f, 1.0f, BREW_LIST_HEAT, 1, BREW_TUNE_HEAT_DELAY },
    { "COOL \xC2\xB7 DIFFERENTIAL",  0.1f, 5.0f, 0.1f,   0.1f, 3.0f, 0.1f,  BREW_LIST_COOL, 0, BREW_TUNE_COOL_DIFF },
    { "COOL \xC2\xB7 RESTART DELAY", 0.0f, 30.0f, 1.0f,  0.0f, 30.0f, 1.0f, BREW_LIST_COOL, 1, BREW_TUNE_COOL_DELAY },
    { "COOL \xC2\xB7 COLD CRASH",    28.0f, 50.0f, 0.5f, -2.0f, 10.0f, 0.5f, BREW_LIST_COOL, 2, BREW_TUNE_CRASH },
    { "TEMPERATURE",                 30.0f, 80.0f, 0.5f, -1.0f, 26.5f, 0.5f, BREW_LIST_STEP, 0, BREW_TUNE_N },
    { "RAMP",                        0.0f, 14.0f, 1.0f,  0.0f, 14.0f, 1.0f, BREW_LIST_STEP, 1, BREW_TUNE_N },
    { "HOLD",                        1.0f, 60.0f, 1.0f,  1.0f, 60.0f, 1.0f, BREW_LIST_STEP, 2, BREW_TUNE_N },
};

static bool is_delay(brew_edit_t e) { return e == BREW_EDIT_HEAT_DELAY || e == BREW_EDIT_COOL_DELAY; }
static bool is_days(brew_edit_t e) { return e == BREW_EDIT_STEP_RAMP || e == BREW_EDIT_STEP_HOLD; }
static bool is_diff(brew_edit_t e) { return e == BREW_EDIT_HEAT_DIFF || e == BREW_EDIT_COOL_DIFF; }

static float snap(float v, float step) { return roundf(v / step) * step; }

/* The current setting, in the units the editor shows. */
static float edit_start(const brew_model_t * m, brew_edit_t e)
{
    switch(e) {
        case BREW_EDIT_HEAT_DIFF:  return m->celsius ? m->heat_diff_f * 5.0f / 9.0f : m->heat_diff_f;
        case BREW_EDIT_COOL_DIFF:  return m->celsius ? m->cool_diff_f * 5.0f / 9.0f : m->cool_diff_f;
        case BREW_EDIT_HEAT_DELAY: return (float)((m->heat_delay_s + 30) / 60);
        case BREW_EDIT_COOL_DELAY: return (float)((m->cool_delay_s + 30) / 60);
        case BREW_EDIT_CRASH:      return brew_model_to_disp(m, m->crash_default_f);
        case BREW_EDIT_STEP_TEMP:  return brew_model_to_disp(m, m->sched.steps[m->step_sel].temp_f);
        case BREW_EDIT_STEP_RAMP:  return (float)((m->sched.steps[m->step_sel].ramp_h + 12) / 24);
        case BREW_EDIT_STEP_HOLD:  return (float)((m->sched.steps[m->step_sel].hold_h + 12) / 24);
        default:                   return 0.0f;
    }
}

static void edit_range(const brew_model_t * m, brew_edit_t e, float * lo, float * hi, float * step)
{
    const edit_def_t * d = &s_edit[e];
    const bool c = m->celsius && !is_delay(e) && !is_days(e);
    *lo = c ? d->min_c : d->min_f;
    *hi = c ? d->max_c : d->max_f;
    *step = c ? d->step_c : d->step_f;
}

void brew_model_edit_text(const brew_model_t * m, brew_edit_text_t * t)
{
    memset(t, 0, sizeof(*t));
    const brew_edit_t e = m->edit < BREW_EDIT_N ? m->edit : BREW_EDIT_HEAT_DIFF;
    float lo, hi, step;
    edit_range(m, e, &lo, &hi, &step);
    const float v = m->edit_val;
    if(e >= BREW_EDIT_STEP_TEMP) snprintf(t->title, sizeof(t->title), "STEP %d \xC2\xB7 %s", m->step_sel + 1, s_edit[e].title);
    else snprintf(t->title, sizeof(t->title), "%s", s_edit[e].title);
    snprintf(t->foot, sizeof(t->foot), "TURN TO CHANGE \xC2\xB7 PRESS TO SAVE");
    t->frac = hi > lo ? (v - lo) / (hi - lo) : 0.0f;
    char d[16];
    if(is_delay(e)) {
        snprintf(t->value, sizeof(t->value), "%d", (int)lroundf(v));
        snprintf(t->unit, sizeof(t->unit), "min");
        const int mins = (int)lroundf(v);
        if(mins == 0) snprintf(t->explain, sizeof(t->explain), "%s may switch back on right away", e == BREW_EDIT_HEAT_DELAY ? "Heat" : "Cooling");
        else if(e == BREW_EDIT_HEAT_DELAY) snprintf(t->explain, sizeof(t->explain), "Once off, heat waits %d min to start again", mins);
        else snprintf(t->explain, sizeof(t->explain), "A rest for the compressor: %d min after it stops", mins);
        return;
    }
    if(is_days(e)) {
        const int days = (int)lroundf(v);
        snprintf(t->value, sizeof(t->value), "%d", days);
        snprintf(t->unit, sizeof(t->unit), "%s", days == 1 ? "day" : "days");
        if(e == BREW_EDIT_STEP_HOLD) snprintf(t->explain, sizeof(t->explain), "Holds the temperature this long");
        else if(days == 0) snprintf(t->explain, sizeof(t->explain), "Jumps straight to the new temperature");
        else snprintf(t->explain, sizeof(t->explain), "Moves the target there gradually");
        return;
    }
    snprintf(t->value, sizeof(t->value), "%.1f", (double)v);
    snprintf(t->unit, sizeof(t->unit), "%s", brew_model_unit(m));
    snprintf(d, sizeof(d), "%.1f%s", (double)v, brew_model_unit(m));
    if(e == BREW_EDIT_HEAT_DIFF) snprintf(t->explain, sizeof(t->explain), "Heating starts %s below the target", d);
    else if(e == BREW_EDIT_COOL_DIFF) snprintf(t->explain, sizeof(t->explain), "Cooling starts %s above the target", d);
    else if(e == BREW_EDIT_CRASH) snprintf(t->explain, sizeof(t->explain), "Where Cold crash starts");
    else snprintf(t->explain, sizeof(t->explain), "The step's temperature");
}

static uint32_t open_list(brew_model_t * m, brew_list_t l, int idx)
{
    m->view = BREW_VIEW_LIST;
    m->list = l;
    m->list_idx = idx;
    return BREW_CH_VIEW | BREW_CH_REFRESH;
}

static uint32_t open_edit(brew_model_t * m, brew_edit_t e)
{
    float lo, hi, step;
    edit_range(m, e, &lo, &hi, &step);
    float v = snap(edit_start(m, e), step);
    m->edit = e;
    m->edit_val = v < lo ? lo : v > hi ? hi : v;
    m->view = BREW_VIEW_EDIT;
    return BREW_CH_VIEW | BREW_CH_REFRESH;
}

static uint32_t save_edit(brew_model_t * m)
{
    const brew_edit_t e = m->edit;
    const float v = m->edit_val;
    float out = 0.0f;
    switch(e) {
        case BREW_EDIT_HEAT_DIFF:  out = m->heat_diff_f = m->celsius ? v * 9.0f / 5.0f : v; break;
        case BREW_EDIT_COOL_DIFF:  out = m->cool_diff_f = m->celsius ? v * 9.0f / 5.0f : v; break;
        case BREW_EDIT_HEAT_DELAY: m->heat_delay_s = (int)lroundf(v) * 60; out = (float)m->heat_delay_s; break;
        case BREW_EDIT_COOL_DELAY: m->cool_delay_s = (int)lroundf(v) * 60; out = (float)m->cool_delay_s; break;
        case BREW_EDIT_CRASH:      out = m->crash_default_f = m->celsius ? v * 9.0f / 5.0f + 32.0f : v; break;
        case BREW_EDIT_STEP_TEMP:
        case BREW_EDIT_STEP_RAMP:
        case BREW_EDIT_STEP_HOLD: {
            brew_step_t * st = &m->sched.steps[m->step_sel];
            if(e == BREW_EDIT_STEP_TEMP) st->temp_f = m->celsius ? v * 9.0f / 5.0f + 32.0f : v;
            else if(e == BREW_EDIT_STEP_RAMP) st->ramp_h = (int)lroundf(v) * 24;
            else st->hold_h = (int)lroundf(v) * 24;
            if(m->cbs.on_step) m->cbs.on_step(m->step_sel, st->temp_f, st->ramp_h, st->hold_h, m->cbs.user);
            return open_list(m, s_edit[e].list, s_edit[e].row);
        }
        default: return 0;
    }
    if(m->cbs.on_tuning) m->cbs.on_tuning(s_edit[e].tune, out, m->cbs.user);
    return open_list(m, s_edit[e].list, s_edit[e].row);
}

const char * brew_model_press_hint(const brew_model_t * m)
{
    switch(m->view) {
        case BREW_VIEW_HOME: return m->setting ? "Set" : "Menu";
        case BREW_VIEW_EDIT: return "Save";
        case BREW_VIEW_LIST: {
            brew_list_item_t it;
            brew_model_list_item(m, m->list_idx, &it);
            switch(it.act) {
                case ACT_FERMENT:
                case ACT_CRASH:       return it.mark == BREW_MARK_ACTIVE ? "Close" : "Start";
                case ACT_PAUSE:       return m->paused ? "Resume" : "Pause";
                case ACT_OPEN:        return "Open";
                case ACT_BACK:        return "Back";
                case ACT_UNITS:       return "Switch";
                case ACT_WIFI_SETUP:  return "Set up";
                case ACT_WIFI_FORGET: return "Forget";
                case ACT_BF_CHECK:    return "Check";
                case ACT_BF_PICK:     return "Open";
                case ACT_BF_RUN:      return "Run";
                case ACT_BF_RUN_OK:   return "OK";
                case ACT_EDIT:        return "Edit";
                case ACT_CONSTANT:    return "Use";
                case ACT_STEP:        return "Edit";
                case ACT_ADD_STEP:    return "Add";
                case ACT_USE_SCHEDULE: return "Start";
                case ACT_STOP_SCHEDULE: return "Stop";
                case ACT_REMOVE_STEP: return "Remove";
                case ACT_RESET_DAY:   return "Reset";
                default:              return "";
            }
        }
        case BREW_VIEW_WIFI:
            switch(brew_model_wifi_screen(m)) {
                case BREW_WIFI_SCREEN_JOIN: return "Skip";
                case BREW_WIFI_SCREEN_DONE: return "Done";
                default:                    return "Cancel";
            }
    }
    return "";
}

bool brew_model_can_step(const brew_model_t * m, int dir)
{
    if(dir == 0) return false;
    switch(m->view) {
        case BREW_VIEW_HOME: {
            const float t = m->setting ? m->draft_f : m->sp_f[m->mode];
            return dir > 0 ? t < BREW_T_MAX : t > BREW_T_MIN;
        }
        case BREW_VIEW_LIST: {
            const int i = m->list_idx + (dir > 0 ? 1 : -1);
            return i >= 0 && i < brew_model_list_count(m);
        }
        case BREW_VIEW_EDIT: {
            float lo, hi, step;
            edit_range(m, m->edit, &lo, &hi, &step);
            return dir > 0 ? m->edit_val < hi - step * 0.5f : m->edit_val > lo + step * 0.5f;
        }
        case BREW_VIEW_WIFI:
            return false;
    }
    return false;
}

/* ------------------------------------------------------------------ WiFi */

static bool setup_phase(brew_wifi_phase_t p)
{
    return p == BREW_WIFI_SETUP || p == BREW_WIFI_PHONE || p == BREW_WIFI_CONNECTING;
}

bool brew_model_wifi_setup_open(const brew_model_t * m) { return setup_phase(m->wifi_phase); }

brew_wifi_screen_t brew_model_wifi_screen(const brew_model_t * m)
{
    switch(m->wifi_phase) {
        case BREW_WIFI_CONNECTING: return BREW_WIFI_SCREEN_TRYING;
        case BREW_WIFI_CONNECTED:  return BREW_WIFI_SCREEN_DONE;
        case BREW_WIFI_PHONE:      return m->wifi_error ? BREW_WIFI_SCREEN_FAILED : BREW_WIFI_SCREEN_PHONE;
        default:                   return m->wifi_error ? BREW_WIFI_SCREEN_FAILED : BREW_WIFI_SCREEN_JOIN;
    }
}

void brew_model_wifi_text(const brew_model_t * m, brew_wifi_text_t * t)
{
    memset(t, 0, sizeof(*t));
    switch(brew_model_wifi_screen(m)) {
        case BREW_WIFI_SCREEN_JOIN:
            snprintf(t->pill, sizeof(t->pill), "WIFI SETUP");
            snprintf(t->title, sizeof(t->title), "Scan to join");
            snprintf(t->line1, sizeof(t->line1), "%s \xC2\xB7 %s", m->wifi_ap_ssid, m->wifi_ap_password);
            snprintf(t->foot, sizeof(t->foot), "PRESS TO SKIP");
            /* the standard WiFi QR format phones understand (our names need no escaping) */
            snprintf(t->qr, sizeof(t->qr), "WIFI:T:WPA;S:%s;P:%s;;", m->wifi_ap_ssid, m->wifi_ap_password);
            break;
        case BREW_WIFI_SCREEN_PHONE:
            snprintf(t->pill, sizeof(t->pill), "WIFI SETUP");
            snprintf(t->title, sizeof(t->title), "Finish on your phone");
            snprintf(t->line1, sizeof(t->line1), "Page didn't open? Go to");
            snprintf(t->line2, sizeof(t->line2), "192.168.4.1");
            snprintf(t->foot, sizeof(t->foot), "PRESS TO CANCEL");
            break;
        case BREW_WIFI_SCREEN_TRYING:
            snprintf(t->pill, sizeof(t->pill), "CONNECTING");
            snprintf(t->title, sizeof(t->title), "%s", m->wifi_ssid);
            snprintf(t->line1, sizeof(t->line1), "Joining your WiFi...");
            snprintf(t->foot, sizeof(t->foot), "PRESS TO CANCEL");
            break;
        case BREW_WIFI_SCREEN_FAILED:
            snprintf(t->pill, sizeof(t->pill), "COULDN'T CONNECT");
            snprintf(t->title, sizeof(t->title), "%s",
                     m->wifi_error == BREW_WIFI_ERR_PASSWORD    ? "Wrong password"
                     : m->wifi_error == BREW_WIFI_ERR_NOT_FOUND ? "Network not found"
                                                                : "Couldn't connect");
            snprintf(t->line1, sizeof(t->line1), "for %s", m->wifi_ssid);
            snprintf(t->line2, sizeof(t->line2), "Try again on your phone");
            snprintf(t->foot, sizeof(t->foot), "PRESS TO CANCEL");
            break;
        case BREW_WIFI_SCREEN_DONE:
            snprintf(t->pill, sizeof(t->pill), "CONNECTED");
            snprintf(t->title, sizeof(t->title), "%s", m->wifi_ssid);
            snprintf(t->line1, sizeof(t->line1), "Saved for next time.");
            snprintf(t->foot, sizeof(t->foot), "PRESS TO CONTINUE");
            break;
    }
}

uint32_t brew_model_set_brewfather(brew_model_t * m, const brew_bf_t * b)
{
    m->bf_account = b->account;
    m->bf_logging = b->logging;
    m->bf_attached = b->attached;
    snprintf(m->bf_batch, sizeof(m->bf_batch), "%s", b->batch ? b->batch : "");
    m->bf_last_log_min = b->last_log_min;
    m->bf_nbatches = b->nbatches < 0 ? 0 : (b->nbatches > BREW_BF_BATCHES_MAX ? BREW_BF_BATCHES_MAX : b->nbatches);
    for(int i = 0; i < m->bf_nbatches; i++) snprintf(m->bf_batches[i], sizeof(m->bf_batches[i]), "%s", b->batches[i] ? b->batches[i] : "");
    m->bf_preview = b->preview;
    snprintf(m->bf_preview_label, sizeof(m->bf_preview_label), "%s", b->preview_label ? b->preview_label : "");
    m->bf_preview_total = b->preview_total;
    m->bf_preview_n = b->preview_n < 0 ? 0 : (b->preview_n > BREW_STEPS_MAX ? BREW_STEPS_MAX : b->preview_n);
    for(int i = 0; i < m->bf_preview_n; i++) {
        m->bf_preview_steps[i].temp_f = b->preview_steps[i].temp_f;
        m->bf_preview_steps[i].ramp_h = b->preview_steps[i].ramp_h;
        m->bf_preview_steps[i].hold_h = b->preview_steps[i].hold_h;
        snprintf(m->bf_preview_types[i], sizeof(m->bf_preview_types[i]), "%s", b->preview_steps[i].type ? b->preview_steps[i].type : "");
    }
    m->bf_today_day = b->today_day;
    m->bf_today_step = b->today_step;
    return BREW_CH_REFRESH;
}

uint32_t brew_model_set_wifi(brew_model_t * m, const brew_wifi_t * w)
{
    const brew_wifi_phase_t was = m->wifi_phase;
    const bool was_open = setup_phase(was);
    m->wifi_phase = w->phase;
    m->wifi_error = w->error;
    snprintf(m->wifi_ssid, sizeof(m->wifi_ssid), "%s", w->ssid ? w->ssid : "");
    snprintf(m->wifi_ap_ssid, sizeof(m->wifi_ap_ssid), "%s", w->ap_ssid ? w->ap_ssid : "");
    snprintf(m->wifi_ap_password, sizeof(m->wifi_ap_password), "%s", w->ap_password ? w->ap_password : "");
    snprintf(m->wifi_address, sizeof(m->wifi_address), "%s", w->address ? w->address : "");
    const bool open = setup_phase(w->phase);

    uint32_t ch = BREW_CH_REFRESH;
    if(open && !was_open && m->view != BREW_VIEW_WIFI) {
        /* setup started (first start, or the menu): show it; a half-made adjustment is dropped */
        const bool dropped = m->setting;
        m->setting = false;
        ch |= set_view(m, BREW_VIEW_WIFI) | (dropped ? BREW_CH_DISARM_COMMIT : 0u);
    }
    else if(m->view == BREW_VIEW_WIFI && !open) {
        if(w->phase == BREW_WIFI_CONNECTED && was == BREW_WIFI_CONNECTING) ch |= BREW_CH_ARM_WIFI_DONE;   /* show "Connected" */
        else if(w->phase != BREW_WIFI_CONNECTED || was != BREW_WIFI_CONNECTED) ch |= set_view(m, BREW_VIEW_HOME);   /* setup closed */
    }
    return ch;
}

uint32_t brew_model_wifi_done(brew_model_t * m)
{
    if(m->view != BREW_VIEW_WIFI || m->wifi_phase != BREW_WIFI_CONNECTED) return 0;
    return set_view(m, BREW_VIEW_HOME);
}

/* ------------------------------------------------------------------ controller -> model */

void brew_model_temp_text(const brew_model_t * m, char * buf, size_t n)
{
    if(m->setting) brew_model_fmt_temp(m, buf, n, m->draft_f);
    else if(!m->temp_known) snprintf(buf, n, "--");   /* never a made-up number */
    else brew_model_fmt_temp(m, buf, n, m->temp_f);
}

uint32_t brew_model_set_no_reading(brew_model_t * m)
{
    if(!m->temp_known) return 0;
    m->temp_known = false;
    return BREW_CH_TEMP | BREW_CH_REFRESH | BREW_CH_GRAPH;
}

uint32_t brew_model_set_temp(brew_model_t * m, float f)
{
    const bool first = !m->temp_known;
    m->temp_f = f;
    m->temp_known = true;
    /* the first reading makes the live point appear: draw the graph now */
    return BREW_CH_TEMP | BREW_CH_REFRESH | BREW_CH_ANIMATE | (first ? BREW_CH_GRAPH : 0u);
}

uint32_t brew_model_set_action(brew_model_t * m, brew_action_t a)
{
    m->action = a;
    return BREW_CH_REFRESH | BREW_CH_ANIMATE;
}

uint32_t brew_model_set_pending(brew_model_t * m, brew_action_t wanted, int wait_s)
{
    /* whole minutes, rounded up: the status only redraws when the shown number changes */
    const int min = wait_s > 0 ? (wait_s + 59) / 60 : 1;
    if(wanted == m->wanted && min == m->wait_min) return 0;
    m->wanted = wanted;
    m->wait_min = min;
    return BREW_CH_REFRESH;
}

uint32_t brew_model_set_setpoints(brew_model_t * m, float ferment_f, float crash_f)
{
    m->sp_f[0] = ferment_f;
    m->sp_f[1] = crash_f;
    return BREW_CH_GRAPH | BREW_CH_REFRESH;
}

uint32_t brew_model_set_mode(brew_model_t * m, brew_mode_t mode, bool paused)
{
    m->mode = mode;
    m->paused = paused;
    return BREW_CH_GRAPH | BREW_CH_REFRESH | BREW_CH_ANIMATE;
}

uint32_t brew_model_set_units(brew_model_t * m, bool celsius)
{
    m->celsius = celsius;
    return BREW_CH_REFRESH;
}

uint32_t brew_model_set_graph_style(brew_model_t * m, brew_graph_style_t style)
{
    if((int)style < 0 || style >= BREW_GRAPH_STYLE_N) style = BREW_GRAPH_HAIRLINE;
    if(style == m->graph_style) return 0;
    m->graph_style = style;
    return BREW_CH_REFRESH | BREW_CH_GRAPH;
}

uint32_t brew_model_set_demo(brew_model_t * m, bool demo)
{
    if(m->demo == demo) return 0;
    m->demo = demo;
    return BREW_CH_REFRESH;
}

uint32_t brew_model_set_batch(brew_model_t * m, const char * name, int day)
{
    snprintf(m->batch, sizeof(m->batch), "%s", name ? name : "");
    m->day = day;
    return BREW_CH_REFRESH;
}

uint32_t brew_model_set_history(brew_model_t * m, const float * t, int n, int ms_per_sample)
{
    if(n > BREW_HIST_MAX) {
        t += n - BREW_HIST_MAX;
        n = BREW_HIST_MAX;
    }
    if(n < 0 || !m->hist) n = 0;
    if(n > 0) memcpy(m->hist, t, (size_t)n * sizeof(float));
    m->hist_n = n;
    if(ms_per_sample > 0) m->hist_ms_per = ms_per_sample;
    return BREW_CH_GRAPH | BREW_CH_REFRESH;
}

uint32_t brew_model_set_schedule(brew_model_t * m, const brew_schedule_t * s)
{
    m->sched = *s;
    if(m->sched.nsteps > BREW_STEPS_MAX) m->sched.nsteps = BREW_STEPS_MAX;
    if(m->sched.nsteps < 0) m->sched.nsteps = 0;
    if(m->list == BREW_LIST_STEP && m->step_sel >= m->sched.nsteps && m->view == BREW_VIEW_LIST)   /* that step is gone */
        return open_list(m, BREW_LIST_SCHEDULE, 0);
    return BREW_CH_REFRESH;
}

uint32_t brew_model_set_tuning(brew_model_t * m, float heat_diff_f, float cool_diff_f, int heat_delay_s, int cool_delay_s,
                               float crash_default_f)
{
    m->heat_diff_f = heat_diff_f;
    m->cool_diff_f = cool_diff_f;
    m->heat_delay_s = heat_delay_s;
    m->cool_delay_s = cool_delay_s;
    m->crash_default_f = crash_default_f;
    return BREW_CH_REFRESH;
}

uint32_t brew_model_push_history(brew_model_t * m, float f)
{
    if(!m->hist) return 0;
    if(m->hist_n == BREW_HIST_MAX) {
        memmove(m->hist, m->hist + 1, (BREW_HIST_MAX - 1) * sizeof(float));
        m->hist_n--;
    }
    m->hist[m->hist_n++] = f;
    return BREW_CH_GRAPH_LAZY;
}

/* ------------------------------------------------------------------ input -> model */

static uint32_t set_view(brew_model_t * m, brew_view_t v)
{
    if(v == BREW_VIEW_LIST) return open_list(m, BREW_LIST_MENU, 0);
    m->view = v;
    return BREW_CH_VIEW | BREW_CH_REFRESH;
}

uint32_t brew_model_commit(brew_model_t * m)
{
    if(!m->setting) return 0;
    m->setting = false;
    m->sp_f[m->mode] = m->draft_f;
    if(m->cbs.on_setpoint) m->cbs.on_setpoint(m->mode, m->draft_f, m->cbs.user);
    return BREW_CH_DISARM_COMMIT | BREW_CH_GRAPH | BREW_CH_REFRESH | BREW_CH_ANIMATE;
}

uint32_t brew_model_encoder(brew_model_t * m, int d)
{
    if(d == 0) return 0;
    switch(m->view) {
        case BREW_VIEW_HOME: {
            if(!m->setting) {
                m->setting = true;
                m->draft_f = m->sp_f[m->mode];
            }
            /* half-degree steps in the units on screen */
            if(m->celsius) {
                float c = (m->draft_f - 32.0f) * 5.0f / 9.0f;
                c = roundf(c * 2.0f) / 2.0f + 0.5f * (float)d;
                m->draft_f = c * 9.0f / 5.0f + 32.0f;
            }
            else {
                m->draft_f = roundf(m->draft_f * 2.0f) / 2.0f + 0.5f * (float)d;
            }
            if(m->draft_f < BREW_T_MIN) m->draft_f = BREW_T_MIN;
            if(m->draft_f > BREW_T_MAX) m->draft_f = BREW_T_MAX;
            return BREW_CH_ARM_COMMIT | BREW_CH_REFRESH | BREW_CH_ANIMATE;
        }
        case BREW_VIEW_LIST: {
            const int n = brew_model_list_count(m);
            int i = m->list_idx + d;
            if(i < 0) i = 0;
            if(i > n - 1) i = n - 1;
            if(i == m->list_idx) return 0;
            m->list_idx = i;
            return BREW_CH_MENU_MOVE;
        }
        case BREW_VIEW_EDIT: {
            float lo, hi, step;
            edit_range(m, m->edit, &lo, &hi, &step);
            float v = snap(m->edit_val + (float)d * step, step);
            if(v < lo) v = lo;
            if(v > hi) v = hi;
            if(fabsf(v - m->edit_val) < step * 0.25f) return 0;
            m->edit_val = v;
            return BREW_CH_REFRESH | BREW_CH_ANIMATE;
        }
        case BREW_VIEW_WIFI:
            return 0;   /* turning does nothing here; a press skips or closes */
    }
    return 0;
}

static uint32_t press_item(brew_model_t * m, const brew_list_item_t * it)
{
    switch(it->act) {
        case ACT_FERMENT:
        case ACT_CRASH:
        {
            const brew_mode_t was = m->mode;
            m->mode = it->act == ACT_FERMENT ? BREW_MODE_FERMENT : BREW_MODE_CRASH;
            m->paused = false;
            /* Cold crash starts at its setting; picking it again mid-crash keeps the adjusted target */
            if(m->mode == BREW_MODE_CRASH && was != BREW_MODE_CRASH) m->sp_f[1] = m->crash_default_f;
            if(m->cbs.on_mode) m->cbs.on_mode(m->mode, m->paused, m->cbs.user);
            return BREW_CH_GRAPH | set_view(m, BREW_VIEW_HOME);
        }
        case ACT_PAUSE:
            m->paused = !m->paused;
            if(m->cbs.on_mode) m->cbs.on_mode(m->mode, m->paused, m->cbs.user);
            return set_view(m, BREW_VIEW_HOME);
        case ACT_OPEN:
            return open_list(m, (brew_list_t)it->arg, 0);
        case ACT_BACK: {
            brew_list_t pl;
            int pi;
            parent(m, m->list, &pl, &pi);
            return pl == BREW_LIST_N ? set_view(m, BREW_VIEW_HOME) : open_list(m, pl, pi);
        }
        case ACT_UNITS:
            m->celsius = !m->celsius;
            if(m->cbs.on_units) m->cbs.on_units(m->celsius, m->cbs.user);
            return BREW_CH_REFRESH;
        case ACT_WIFI_SETUP:
            /* WifiSetup opens setup; its next status brings up the WiFi view */
            if(m->cbs.on_wifi_setup) m->cbs.on_wifi_setup(m->cbs.user);
            return set_view(m, BREW_VIEW_HOME);
        case ACT_WIFI_FORGET:
            if(m->cbs.on_wifi_forget) m->cbs.on_wifi_forget(m->cbs.user);
            return set_view(m, BREW_VIEW_HOME);
        case ACT_BF_CHECK:
            if(m->cbs.on_bf_check) m->cbs.on_bf_check(m->cbs.user);
            return BREW_CH_REFRESH;
        case ACT_BF_PICK:
            m->bf_pick = it->arg;
            m->bf_preview = BREW_BF_PREVIEW_LOADING;   /* until it arrives: never the last batch's steps */
            snprintf(m->bf_preview_label, sizeof(m->bf_preview_label), "%s", m->bf_batches[it->arg]);
            if(m->cbs.on_bf_pick) m->cbs.on_bf_pick(it->arg, m->cbs.user);
            return open_list(m, BREW_LIST_BF_PREVIEW, 0);
        case ACT_BF_RUN:
            if(m->bf_preview_total > BREW_STEPS_MAX) return open_list(m, BREW_LIST_BF_NOTICE, 0);
            /* fall through */
        case ACT_BF_RUN_OK:
            if(m->cbs.on_bf_run) m->cbs.on_bf_run(m->cbs.user);
            return set_view(m, BREW_VIEW_HOME);
        case ACT_EDIT:
            return open_edit(m, (brew_edit_t)it->arg);
        case ACT_CONSTANT:
        case ACT_STOP_SCHEDULE:
            if(m->sched.profile != BREW_PROFILE_CONSTANT) {
                m->sched.profile = BREW_PROFILE_CONSTANT;
                if(m->cbs.on_profile) m->cbs.on_profile(BREW_PROFILE_CONSTANT, m->cbs.user);
            }
            return BREW_CH_REFRESH | BREW_CH_GRAPH;
        case ACT_USE_SCHEDULE:
            m->sched.profile = BREW_PROFILE_SCHEDULE;
            m->sched.step = 0;
            m->sched.override_on = false;
            if(m->cbs.on_profile) m->cbs.on_profile(BREW_PROFILE_SCHEDULE, m->cbs.user);
            if(m->mode != BREW_MODE_FERMENT || m->paused) {   /* a schedule runs as Ferment */
                m->mode = BREW_MODE_FERMENT;
                m->paused = false;
                if(m->cbs.on_mode) m->cbs.on_mode(m->mode, m->paused, m->cbs.user);
            }
            return BREW_CH_GRAPH | set_view(m, BREW_VIEW_HOME);
        case ACT_STEP:
            m->step_sel = it->arg;
            return open_list(m, BREW_LIST_STEP, 0);
        case ACT_ADD_STEP: {
            brew_schedule_t * s = &m->sched;
            if(s->nsteps >= BREW_STEPS_MAX) return 0;
            /* the new step starts like the last one; the first, at today's Ferment target for 3 days */
            const brew_step_t st = s->nsteps > 0 ? s->steps[s->nsteps - 1] : (brew_step_t){ m->sp_f[BREW_MODE_FERMENT], 0, 72 };
            s->steps[s->nsteps] = st;
            s->nsteps++;
            m->step_sel = s->nsteps - 1;
            if(m->cbs.on_step) m->cbs.on_step(m->step_sel, st.temp_f, st.ramp_h, st.hold_h, m->cbs.user);
            return open_list(m, BREW_LIST_STEP, 0);   /* straight into the new step */
        }
        case ACT_REMOVE_STEP: {
            brew_schedule_t * s = &m->sched;
            if(s->nsteps < 1) return 0;
            for(int i = m->step_sel; i < s->nsteps - 1; i++) {
                s->steps[i] = s->steps[i + 1];
                if(m->cbs.on_step) m->cbs.on_step(i, s->steps[i].temp_f, s->steps[i].ramp_h, s->steps[i].hold_h, m->cbs.user);
            }
            s->nsteps--;
            if(s->nsteps == 0) s->profile = BREW_PROFILE_CONSTANT;   /* nothing left to run (the controller does the same) */
            if(m->cbs.on_step_count) m->cbs.on_step_count(s->nsteps, m->cbs.user);
            return open_list(m, BREW_LIST_SCHEDULE, m->step_sel < s->nsteps ? m->step_sel : (s->nsteps > 0 ? s->nsteps - 1 : 0));
        }
        case ACT_RESET_DAY:
            m->day = 1;   /* the controller confirms in its next State */
            if(m->cbs.on_reset_day) m->cbs.on_reset_day(m->cbs.user);
            return BREW_CH_REFRESH;
        default:
            return 0;
    }
}

uint32_t brew_model_press(brew_model_t * m)
{
    switch(m->view) {
        case BREW_VIEW_HOME:
            return m->setting ? brew_model_commit(m) : set_view(m, BREW_VIEW_LIST);
        case BREW_VIEW_LIST: {
            brew_list_item_t it;
            brew_model_list_item(m, m->list_idx, &it);
            return press_item(m, &it);
        }
        case BREW_VIEW_EDIT:
            return save_edit(m);
        case BREW_VIEW_WIFI:
            /* skip / cancel setup, or dismiss "Connected"; either way back to the dial */
            if(m->wifi_phase != BREW_WIFI_CONNECTED && m->cbs.on_wifi_cancel) m->cbs.on_wifi_cancel(m->cbs.user);
            return set_view(m, BREW_VIEW_HOME);
    }
    return 0;
}
