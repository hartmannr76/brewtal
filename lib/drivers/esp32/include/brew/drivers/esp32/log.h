/**
 * Serial log lines that always print. The Arduino-ESP32 build compiles ESP-IDF's ESP_LOGx at
 * error level only (CONFIG_LOG_MAXIMUM_LEVEL 1), so ESP_LOGI / ESP_LOGW in library code
 * vanish at compile time; these go straight to the console with printf instead.
 *
 *   [  40738][I][brew.wifi] gave the phone address 192.168.4.2
 */
#pragma once

#include "esp_timer.h"

#include <cstdio>

#define BREW_LOG_(lvl, tag, fmt, ...)                                                                   \
    std::printf("[%7u][" lvl "][%s] " fmt "\n", static_cast<unsigned>(esp_timer_get_time() / 1000), tag, \
                ##__VA_ARGS__)
#define BREW_LOGI(tag, fmt, ...) BREW_LOG_("I", tag, fmt, ##__VA_ARGS__)
#define BREW_LOGW(tag, fmt, ...) BREW_LOG_("W", tag, fmt, ##__VA_ARGS__)
#define BREW_LOGE(tag, fmt, ...) BREW_LOG_("E", tag, fmt, ##__VA_ARGS__)
