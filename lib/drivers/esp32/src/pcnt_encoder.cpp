#include "brew/drivers/esp32/pcnt_encoder.h"

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"

#include <cstdio>
#include <cstring>

namespace brew::drivers {

/* Elecrow's sample polled GPIO 42 every 2 ms and counted only rising edges of A. The EC28A has
 * 30 detents but 15 pulses per turn, so each click is half a quadrature cycle and that missed
 * every other click. PCNT decodes both channels (x4) in hardware instead: no missed edges,
 * and bounce on one channel just toggles the count +/-1. */
bool EspPcntEncoder::begin()
{
    pcnt_unit_config_t uc = {};
    uc.low_limit = -30000;
    uc.high_limit = 30000;
    uc.flags.accum_count = 1;   // keep counting past the limits
    pcnt_unit_handle_t u = nullptr;
    if(pcnt_new_unit(&uc, &u) != ESP_OK) return false;

    pcnt_glitch_filter_config_t f = {};
    f.max_glitch_ns = cfg_.glitch_ns;
    pcnt_unit_set_glitch_filter(u, &f);

    pcnt_chan_config_t ca = {};
    ca.edge_gpio_num = cfg_.pin_a;
    ca.level_gpio_num = cfg_.pin_b;
    pcnt_chan_config_t cb = {};
    cb.edge_gpio_num = cfg_.pin_b;
    cb.level_gpio_num = cfg_.pin_a;
    pcnt_channel_handle_t cha = nullptr, chb = nullptr;
    if(pcnt_new_channel(u, &ca, &cha) != ESP_OK || pcnt_new_channel(u, &cb, &chb) != ESP_OK) return false;
    pcnt_channel_set_edge_action(cha, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(cha, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(chb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(chb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_unit_add_watch_point(u, uc.low_limit);
    pcnt_unit_add_watch_point(u, uc.high_limit);

    gpio_set_pull_mode(static_cast<gpio_num_t>(cfg_.pin_a), GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(static_cast<gpio_num_t>(cfg_.pin_b), GPIO_PULLUP_ONLY);

    if(pcnt_unit_enable(u) != ESP_OK || pcnt_unit_clear_count(u) != ESP_OK || pcnt_unit_start(u) != ESP_OK) return false;
    unit_ = u;
    return true;
}

int32_t EspPcntEncoder::count()
{
    int c = 0;
    if(unit_) pcnt_unit_get_count(static_cast<pcnt_unit_handle_t>(unit_), &c);
    /* With this channel setup, A rising while B is low counts up = counter-clockwise. */
    return cfg_.reverse ? c : -c;
}

}  // namespace brew::drivers
