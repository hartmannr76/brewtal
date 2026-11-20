/**
 * Redraw benchmark: how many pixels LVGL re-renders (and how many flushes) for
 * each kind of interaction. Pixel count is the main cost driver on the ESP32.
 * Usage: ./brew_bench
 */
#include "lvgl.h"
#include "brew_ui.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#define N BREW_UI_DISP_SIZE

static uint32_t s_ms;
static uint64_t s_px;
static uint32_t s_flushes;
static double s_render_s;

static uint32_t tick_cb(void) { return s_ms; }

static void flush_cb(lv_display_t * d, const lv_area_t * a, uint8_t * px)
{
    (void)px;
    s_px += (uint64_t)lv_area_get_width(a) * (uint64_t)lv_area_get_height(a);
    s_flushes++;
    lv_display_flush_ready(d);
}

static double now_s(void)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);   /* C11: works with MSVC, MinGW and glibc */
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static void run(uint32_t ms)
{
    for(uint32_t t = 0; t < ms; t += 5) {
        s_ms += 5;
        const double t0 = now_s();
        lv_timer_handler();
        s_render_s += now_s() - t0;
    }
}

static void measure_pre(const char * name, void (*act)(void), uint32_t settle_ms, uint32_t pre_ms)
{
    run(pre_ms);                     /* let everything settle first */
    s_px = 0; s_flushes = 0; s_render_s = 0;
    if(act) act();
    run(settle_ms);
    printf("  %-34s %9.2f screens  %5u flushes  %7.1f ms cpu (host)\n", name,
           (double)s_px / (double)(N * N), s_flushes, s_render_s * 1000.0);
}

static void measure(const char * name, void (*act)(void), uint32_t settle_ms) { measure_pre(name, act, settle_ms, 3000); }

static float g_temp = 68.4f;
static void a_idle(void) {}
static void a_temp(void) { g_temp -= 0.1f; brew_ui_set_temp(g_temp); }
static void a_turn(void) { brew_ui_encoder(1); }
static void a_turn5(void) { for(int i = 0; i < 5; i++) { brew_ui_encoder(1); s_ms += 80; lv_timer_handler(); } }
static void a_press(void) { brew_ui_press(); }
static void a_state(void) { brew_ui_set_action(BREW_ACTION_IDLE); }

int main(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t * d = lv_display_create(N, N);
    static uint16_t buf[N * 48];
    lv_display_set_buffers(d, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush_cb);

    brew_ui_create(lv_screen_active(), NULL);
    brew_ui_set_batch("Pale ale", 4);
    brew_ui_set_setpoints(66, 34);
    float h[48];
    for(int i = 0; i < 48; i++) h[i] = 66 + 2.4f * (1 - i / 47.0f) + 0.5f * sinf(i * 0.4f);
    h[47] = g_temp;
    brew_ui_set_history(h, 48, 30 * 60);
    brew_ui_set_temp(g_temp);
    brew_ui_set_action(BREW_ACTION_COOLING);

    printf("Redraw cost per interaction (%dx%d; 1 screen = %d px)\n", N, N, N * N);
    measure("idle 2 s",                    a_idle, 2000);
    measure("temperature tick (-0.1)",     a_temp, 2500);
    measure("crown: first detent",         a_turn, 500);
    measure_pre("crown: 5 more detents (adjusting)", a_turn5, 500, 0);
    measure("auto-commit after idle",      NULL, 3500);
    measure("open menu",                   a_press, 600);
    measure("menu: scroll 1",              a_turn, 500);
    measure("menu: back to home (Ferment)",a_press, 800);
    measure("state change (cool->hold)",   a_state, 1200);
    return 0;
}
