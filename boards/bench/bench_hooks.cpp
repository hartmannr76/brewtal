/**
 * The UI benchmark's wiring for bench builds. Env crowpanel21_bench adds this folder to its
 * sources and brewdial-bench to its libraries; no other build compiles any of it. The board's
 * main.cpp points DisplayConfig::ui_started / ui_loop here when BREW_BENCH is set.
 * tools/device_bench.py flashes the build and reads the BENCH lines.
 */
#include "brew/app/ui_bridge.h"
#include "brew/bench/bench.h"
#include "brew/network/wifi_setup.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <cstdio>

namespace {
brew::bench::FrameMeter s_meter;
brew::bench::Bench * s_bench = nullptr;

int64_t micros() { return esp_timer_get_time(); }
void log_line(const char * line)
{
    std::printf("[%7u][I][brewdial] %s\n", static_cast<unsigned>(esp_timer_get_time() / 1000), line);
}
}  // namespace

void benchUiStarted(brew::app::UiBridge & ui, lv_display_t * disp, brew::net::WifiSetup * wifi)
{
    s_meter.attach(disp, micros);
    s_bench = new brew::bench::Bench(ui, s_meter, log_line, wifi);
}

void benchUiLoop(uint32_t now_ms)
{
    if(s_bench) s_bench->step(now_ms);
}
