#include "brew/drivers/esp32/ledc_backlight.h"

#include "driver/ledc.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>

namespace brew::drivers {

bool EspLedcBacklight::begin()
{
    ledc_timer_config_t t = {};
    t.speed_mode = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_8_BIT;
    t.timer_num = static_cast<ledc_timer_t>(cfg_.timer);
    t.freq_hz = cfg_.freq_hz;
    t.clk_cfg = LEDC_AUTO_CLK;
    if(ledc_timer_config(&t) != ESP_OK) return false;
    ledc_channel_config_t c = {};
    c.gpio_num = cfg_.pin;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel = static_cast<ledc_channel_t>(cfg_.channel);
    c.timer_sel = static_cast<ledc_timer_t>(cfg_.timer);
    c.duty = cfg_.active_low ? 255 : 0;   // start dark
    c.flags.output_invert = 0;
    ready_ = ledc_channel_config(&c) == ESP_OK;
    return ready_;
}

void EspLedcBacklight::set(uint8_t level)
{
    if(!ready_ && !begin()) return;
    const uint32_t duty = cfg_.active_low ? 255u - level : level;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(cfg_.channel), duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, static_cast<ledc_channel_t>(cfg_.channel));
}

}  // namespace brew::drivers
