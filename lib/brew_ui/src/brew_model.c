/**
 * brew_model.c - the UI's state and rules (see brew_model.h). No LVGL here: this file is
 * compiled into the host unit tests on its own.
 */
#include "brew_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void brew_model_init(brew_model_t * m, const brew_ui_cbs_t * cbs)
{
    memset(m, 0, sizeof(*m));
    if(cbs) m->cbs = *cbs;
    m->temp_f = 66.0f;
    m->sp_f[0] = 66.0f;
    m->sp_f[1] = 34.0f;
    m->hist_sec_per = 2;
    m->view = BREW_VIEW_HOME;
}

/* ------------------------------------------------------------------ derived */

brew_vstate_t brew_model_vstate(const brew_model_t * m)
{
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

void brew_model_status_text(const brew_model_t * m, char * buf, size_t n)
{
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

int brew_model_hist_last(const brew_model_t * m) { return m->temp_known ? m->hist_n : m->hist_n - 1; }

float brew_model_hist_val(const brew_model_t * m, int i) { return i < m->hist_n ? m->hist[i] : m->temp_f; }

void brew_model_hist_time(const brew_model_t * m, int idx, char * buf, size_t n)
{
    /* sample i covers the interval that ended (hist_n - i) samples ago; hist_n is live */
    const int back_s = (m->hist_n - idx) * m->hist_sec_per;
    const int back_min = back_s / 60;
    if(idx >= m->hist_n || back_s <= 0) {
        snprintf(buf, n, "Now");
        return;
    }
    time_t now = time(NULL);
    if(now > 1700000000) { /* wall clock is set (SNTP) */
        time_t t = now - (time_t)back_s;
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
    else if(back_s < 60) {
        snprintf(buf, n, "%d s ago", back_s);
    }
    else {
        snprintf(buf, n, "%dh %02dm ago", back_min / 60, back_min % 60);
    }
}

int brew_model_hist_step(const brew_model_t * m)
{
    const int s = m->hist_sec_per > 0 ? BREW_SCRUB_S / m->hist_sec_per : 1;
    return s > 0 ? s : 1;
}

void brew_model_hist_window_text(const brew_model_t * m, char * buf, size_t n)
{
    const int min = BREW_HIST_MAX * (m->hist_sec_per > 0 ? m->hist_sec_per : 1) / 60;
    if(min < 60) snprintf(buf, n, "last %d min", min);
    else if(min == 60) snprintf(buf, n, "last hour");
    else if(min % 60 == 0) snprintf(buf, n, "last %d hours", min / 60);
    else snprintf(buf, n, "last %dh %02dm", min / 60, min % 60);
}

void brew_model_menu_label(const brew_model_t * m, int i, char * buf, size_t n)
{
    switch(i) {
        case BREW_MENU_FERMENT: snprintf(buf, n, "Ferment"); break;
        case BREW_MENU_CRASH:   snprintf(buf, n, "Cold crash"); break;
        case BREW_MENU_PAUSE:   snprintf(buf, n, "%s", m->paused ? "Resume" : "Pause"); break;
        case BREW_MENU_HISTORY: snprintf(buf, n, "History"); break;
        case BREW_MENU_UNITS:   snprintf(buf, n, "Units \xC2\xB7 \xC2\xB0%c", m->celsius ? 'C' : 'F'); break;
        default:                if(n) buf[0] = 0; break;
    }
}

bool brew_model_menu_active(const brew_model_t * m, int i)
{
    return !m->paused && ((i == BREW_MENU_FERMENT && m->mode == BREW_MODE_FERMENT) ||
                          (i == BREW_MENU_CRASH && m->mode == BREW_MODE_CRASH));
}

const char * brew_model_press_hint(const brew_model_t * m)
{
    switch(m->view) {
        case BREW_VIEW_HOME: return m->setting ? "Set" : "Menu";
        case BREW_VIEW_HIST: return "Back";
        case BREW_VIEW_MENU:
            switch(m->menu_idx) {
                case BREW_MENU_FERMENT:
                case BREW_MENU_CRASH:   return brew_model_menu_active(m, m->menu_idx) ? "Close" : "Start";
                case BREW_MENU_PAUSE:   return m->paused ? "Resume" : "Pause";
                case BREW_MENU_HISTORY: return "Open";
                case BREW_MENU_UNITS:   return "Switch";
            }
            break;
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
        case BREW_VIEW_MENU: {
            const int i = m->menu_idx + (dir > 0 ? 1 : -1);
            return i >= 0 && i < BREW_MENU_N;
        }
        case BREW_VIEW_HIST:
            return dir > 0 ? m->hist_idx < brew_model_hist_last(m) : m->hist_idx > 0;
    }
    return false;
}

/* ------------------------------------------------------------------ controller -> model */

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

uint32_t brew_model_set_batch(brew_model_t * m, const char * name, int day)
{
    snprintf(m->batch, sizeof(m->batch), "%s", name ? name : "");
    m->day = day;
    return BREW_CH_REFRESH;
}

uint32_t brew_model_set_history(brew_model_t * m, const float * t, int n, int seconds_per_sample)
{
    if(n > BREW_HIST_MAX) {
        t += n - BREW_HIST_MAX;
        n = BREW_HIST_MAX;
    }
    if(n < 0) n = 0;
    if(n > 0) memcpy(m->hist, t, (size_t)n * sizeof(float));
    m->hist_n = n;
    if(seconds_per_sample > 0) m->hist_sec_per = seconds_per_sample;
    if(m->view == BREW_VIEW_HIST) m->hist_idx = brew_model_hist_last(m) > 0 ? brew_model_hist_last(m) : 0;
    return BREW_CH_GRAPH | BREW_CH_REFRESH;
}

uint32_t brew_model_push_history(brew_model_t * m, float f)
{
    const bool was_live = m->hist_idx >= m->hist_n;   /* scrubbing: keep the same moment selected */
    if(m->hist_n == BREW_HIST_MAX) {
        memmove(m->hist, m->hist + 1, (BREW_HIST_MAX - 1) * sizeof(float));
        m->hist_n--;
        if(!was_live && m->hist_idx > 0) m->hist_idx--;
    }
    m->hist[m->hist_n++] = f;
    if(was_live) m->hist_idx = brew_model_hist_last(m) > 0 ? brew_model_hist_last(m) : 0;
    return BREW_CH_GRAPH_LAZY;
}

/* ------------------------------------------------------------------ input -> model */

static uint32_t set_view(brew_model_t * m, brew_view_t v)
{
    m->view = v;
    if(v == BREW_VIEW_MENU) m->menu_idx = 0;
    if(v == BREW_VIEW_HIST) m->hist_idx = brew_model_hist_last(m) > 0 ? brew_model_hist_last(m) : 0;
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
        case BREW_VIEW_MENU: {
            int i = m->menu_idx + d;
            if(i < 0) i = 0;
            if(i > BREW_MENU_N - 1) i = BREW_MENU_N - 1;
            if(i == m->menu_idx) return 0;
            m->menu_idx = i;
            return BREW_CH_MENU_MOVE;
        }
        case BREW_VIEW_HIST: {
            int i = m->hist_idx + d * brew_model_hist_step(m);   /* a minute per detent */
            if(i > brew_model_hist_last(m)) i = brew_model_hist_last(m);
            if(i < 0) i = 0;
            if(i == m->hist_idx) return 0;
            m->hist_idx = i;
            return BREW_CH_REFRESH | BREW_CH_ANIMATE;
        }
    }
    return 0;
}

uint32_t brew_model_press(brew_model_t * m)
{
    switch(m->view) {
        case BREW_VIEW_HOME:
            return m->setting ? brew_model_commit(m) : set_view(m, BREW_VIEW_MENU);
        case BREW_VIEW_HIST:
            return set_view(m, BREW_VIEW_HOME);
        case BREW_VIEW_MENU:
            switch(m->menu_idx) {
                case BREW_MENU_FERMENT:
                case BREW_MENU_CRASH:
                    m->mode = m->menu_idx == BREW_MENU_FERMENT ? BREW_MODE_FERMENT : BREW_MODE_CRASH;
                    m->paused = false;
                    if(m->cbs.on_mode) m->cbs.on_mode(m->mode, m->paused, m->cbs.user);
                    return BREW_CH_GRAPH | set_view(m, BREW_VIEW_HOME);
                case BREW_MENU_PAUSE:
                    m->paused = !m->paused;
                    if(m->cbs.on_mode) m->cbs.on_mode(m->mode, m->paused, m->cbs.user);
                    return set_view(m, BREW_VIEW_HOME);
                case BREW_MENU_HISTORY:
                    return set_view(m, BREW_VIEW_HIST);
                case BREW_MENU_UNITS:
                    m->celsius = !m->celsius;
                    if(m->cbs.on_units) m->cbs.on_units(m->celsius, m->cbs.user);
                    return BREW_CH_REFRESH;
            }
            break;
    }
    return 0;
}
