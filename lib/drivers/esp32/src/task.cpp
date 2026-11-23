#include "brew/drivers/esp32/task.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace brew::drivers {

namespace {
struct Periodic {
    PeriodicFn fn;
    void * ctx;
    uint32_t period_ms;
};

void periodic_task(void * arg)
{
    const Periodic p = *static_cast<Periodic *>(arg);
    delete static_cast<Periodic *>(arg);
    TickType_t wake = xTaskGetTickCount();
    for(;;) {
        p.fn(p.ctx, millis32());
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(p.period_ms ? p.period_ms : 1));
    }
}
}  // namespace

uint32_t millis32()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void startPeriodic(const char * name, PeriodicFn fn, void * ctx, uint32_t period_ms, int core, int priority, uint32_t stack)
{
    xTaskCreatePinnedToCore(periodic_task, name, stack, new Periodic{ fn, ctx, period_ms },
                            static_cast<UBaseType_t>(priority), nullptr, core);
}


void delayMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

}  // namespace brew::drivers
