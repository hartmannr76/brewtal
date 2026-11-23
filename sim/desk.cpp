/**
 * Interactive desktop simulator (SDL2 window, real LVGL rendering in RGB565), running the same
 * Controller + LocalBus + UiBridge stack as the firmware, with a FakePlant fermenter.
 *
 *   Mouse wheel / Left / Right   turn the crown
 *   Click / Space / Enter        press
 *   Up / Down                    nudge the beer temperature +/- 0.5 F
 *   M                            toggle the round mask
 *   Z                            toggle 2x zoom
 *   Esc                          quit
 */
#include "brew/app/ui_bridge.h"
#include "brew/controller.h"
#include "brew/fake_plant.h"
#include "brew/local_bus.h"

#include "lvgl.h"

#define SDL_MAIN_HANDLED   /* we own main(); no SDL2main needed on Windows */
#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>

using namespace brew;

namespace {

constexpr int SIZE = BREW_UI_DISP_SIZE;

/* Input is captured by an SDL event watch (runs inside LVGL's SDL polling) and applied from
 * the main loop, so we never re-enter LVGL from its own handler. */
volatile int s_delta, s_press, s_temp_nudge, s_mask_toggle, s_zoom_toggle, s_quit;

int event_watch(void *, SDL_Event * e)
{
    if(e->type == SDL_MOUSEWHEEL) {
        s_delta += e->wheel.y > 0 ? 1 : (e->wheel.y < 0 ? -1 : 0);
    }
    else if(e->type == SDL_KEYDOWN) {
        switch(e->key.keysym.sym) {
            case SDLK_RIGHT:  s_delta++; break;
            case SDLK_LEFT:   s_delta--; break;
            case SDLK_SPACE:
            case SDLK_RETURN: s_press = 1; break;
            case SDLK_UP:     s_temp_nudge++; break;
            case SDLK_DOWN:   s_temp_nudge--; break;
            case SDLK_m:      s_mask_toggle = 1; break;
            case SDLK_z:      s_zoom_toggle = 1; break;
            case SDLK_ESCAPE: s_quit = 1; break;
            default: break;
        }
    }
    return 0;
}

/* Prints commands so you can see exactly what a real controller would receive. */
class LoggingPort final : public UiPort {
public:
    explicit LoggingPort(UiPort & in) : in_(in) {}
    void send(const Command & c) override
    {
        if(c.kind == Command::Kind::SetSetpoint)
            std::printf("setpoint[%d] = %.1f F\n", static_cast<int>(c.mode), c_to_f(c.setpoint_c));
        else if(c.kind == Command::Kind::SetMode)
            std::printf("mode=%d paused=%d\n", static_cast<int>(c.mode), c.paused);
        else if(c.kind == Command::Kind::SetUnits)
            std::printf("celsius=%d\n", c.celsius);
        in_.send(c);
    }
    bool pollState(State & s) override { return in_.pollState(s); }
    bool pollHistory(HistoryChunk & h) override { return in_.pollHistory(h); }

private:
    UiPort & in_;
};

lv_obj_t * make_round_mask()
{
    /* A ring on the top layer that blacks out the square corners the real panel doesn't have. */
    const int32_t b = SIZE / 4;   /* covers the corners: (sqrt2 - 1) * r < r / 2 */
    lv_obj_t * m = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, SIZE + 2 * b, SIZE + 2 * b);
    lv_obj_center(m);
    lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(m, b, 0);
    lv_obj_set_style_border_color(m, lv_color_hex(0x101214), 0);
    lv_obj_set_style_border_opa(m, LV_OPA_COVER, 0);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_CLICKABLE);
    return m;
}

}  // namespace

int main(int, char **)
{
    SDL_SetMainReady();
    lv_init();
    lv_display_t * disp = lv_sdl_window_create(SIZE, SIZE);
    lv_sdl_window_set_title(disp, "Brewdial");
    lv_sdl_mouse_create();               /* clicks -> taps -> press */
    SDL_AddEventWatch(event_watch, nullptr);

    LocalBus bus;
    LoggingPort port(bus);
    FakePlant plant({});
    HysteresisLaw law(ChamberDefaults::kHeatDiffF * 5.0f / 9.0f, ChamberDefaults::kCoolDiffF * 5.0f / 9.0f);
    OutputGuard guard(&plant.heat(), &plant.cool(), ChamberDefaults::guard());
    Controller::Config cc;
    Controller ctl(bus, plant, law, guard, nullptr, cc);
    ctl.begin(lv_tick_get());

    app::UiBridge bridge(port);
    bridge.create(lv_screen_active());

    lv_obj_t * mask = make_round_mask();
    bool zoom = false;

    /* optional auto-exit for CI: BREW_DESK_EXIT_MS=3000 */
    const char * ex = std::getenv("BREW_DESK_EXIT_MS");
    const uint32_t exit_at = ex ? static_cast<uint32_t>(std::atoi(ex)) : 0;

    while(!s_quit) {
        const int d = s_delta;
        if(d) {
            s_delta -= d;
            bridge.handle(InputEvent::rotate(d));
        }
        if(s_press) {
            s_press = 0;
            bridge.handle(InputEvent::press());
        }
        if(s_temp_nudge) {
            const float delta_f = 0.5f * static_cast<float>(s_temp_nudge);
            plant.set(plant.actual() + delta_f * 5.0f / 9.0f);
            s_temp_nudge = 0;
            ctl.evaluateNow(lv_tick_get());
        }
        if(s_mask_toggle) {
            s_mask_toggle = 0;
            if(lv_obj_has_flag(mask, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(mask, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(mask, LV_OBJ_FLAG_HIDDEN);
        }
        if(s_zoom_toggle) {
            s_zoom_toggle = 0;
            zoom = !zoom;
            lv_sdl_window_set_zoom(disp, zoom ? 2.0f : 1.0f);
        }

        ctl.tick(lv_tick_get());
        bridge.poll(lv_tick_get());
        const uint32_t wait = lv_timer_handler();
        if(exit_at && lv_tick_get() > exit_at) break;
        SDL_Delay(wait < 5 ? 5 : (wait > 16 ? 16 : wait));
    }
    return 0;
}
