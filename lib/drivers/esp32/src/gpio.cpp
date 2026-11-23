#include "brew/drivers/esp32/gpio.h"

#include "driver/gpio.h"

#include <cstdio>
#include <cstring>

namespace brew::drivers {

bool EspGpioOut::begin()
{
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << pin_;
    c.mode = GPIO_MODE_OUTPUT;
    gpio_set_level(static_cast<gpio_num_t>(pin_), initial_);
    return gpio_config(&c) == ESP_OK;
}

void EspGpioOut::write(bool high)
{
    gpio_set_level(static_cast<gpio_num_t>(pin_), high);
}

bool EspGpioIn::begin()
{
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << pin_;
    c.mode = GPIO_MODE_INPUT;
    c.pull_up_en = pullup_ ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    return gpio_config(&c) == ESP_OK;
}

bool EspGpioIn::read()
{
    return gpio_get_level(static_cast<gpio_num_t>(pin_)) != 0;
}

}  // namespace brew::drivers
