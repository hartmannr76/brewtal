/**
 * Tiny FreeRTOS helpers for board main.cpp files (no app or UI dependencies).
 */
#pragma once

#include <cstdint>

namespace brew::drivers {

/** Milliseconds since boot (esp_timer), wraps after ~49 days like everything else here. */
uint32_t millis32();

/** Blocking delay usable from board bring-up code. */
void delayMs(uint32_t ms);

using PeriodicFn = void (*)(void * ctx, uint32_t now_ms);

/** Runs fn(ctx, now) every period_ms on `core`, forever. */
void startPeriodic(const char * name, PeriodicFn fn, void * ctx, uint32_t period_ms, int core = 0,
                   int priority = 4, uint32_t stack = 6144);

}  // namespace brew::drivers
