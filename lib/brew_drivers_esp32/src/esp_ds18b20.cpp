/* DS18B20 on a bit-banged 1-Wire bus (one sensor per pin, SKIP ROM). */
#include "brew/drivers/ds18b20.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace brew::drivers {

namespace {

portMUX_TYPE s_ow_mux = portMUX_INITIALIZER_UNLOCKED;

inline void low(int pin) { gpio_set_level(static_cast<gpio_num_t>(pin), 0); }
inline void release(int pin) { gpio_set_level(static_cast<gpio_num_t>(pin), 1); }   // open drain + pull-up
inline int  sample(int pin) { return gpio_get_level(static_cast<gpio_num_t>(pin)); }

uint8_t crc8(const uint8_t * p, int n)
{
    uint8_t c = 0;
    for(int i = 0; i < n; i++) {
        uint8_t b = p[i];
        for(int k = 0; k < 8; k++) {
            const uint8_t mix = (c ^ b) & 1u;
            c >>= 1;
            if(mix) c ^= 0x8C;
            b >>= 1;
        }
    }
    return c;
}

}  // namespace

bool EspDs18b20::reset()
{
    const int p = cfg_.pin;
    low(p);
    esp_rom_delay_us(480);
    portENTER_CRITICAL(&s_ow_mux);
    release(p);
    esp_rom_delay_us(70);
    const bool present = sample(p) == 0;
    portEXIT_CRITICAL(&s_ow_mux);
    esp_rom_delay_us(410);
    return present;
}

void EspDs18b20::writeByte(uint8_t v)
{
    const int p = cfg_.pin;
    for(int i = 0; i < 8; i++, v >>= 1) {
        portENTER_CRITICAL(&s_ow_mux);
        low(p);
        if(v & 1u) {
            esp_rom_delay_us(6);
            release(p);
            esp_rom_delay_us(64);
        }
        else {
            esp_rom_delay_us(60);
            release(p);
            esp_rom_delay_us(10);
        }
        portEXIT_CRITICAL(&s_ow_mux);
    }
}

uint8_t EspDs18b20::readByte()
{
    const int p = cfg_.pin;
    uint8_t v = 0;
    for(int i = 0; i < 8; i++) {
        portENTER_CRITICAL(&s_ow_mux);
        low(p);
        esp_rom_delay_us(3);
        release(p);
        esp_rom_delay_us(10);
        const int b = sample(p);
        portEXIT_CRITICAL(&s_ow_mux);
        esp_rom_delay_us(53);
        v = static_cast<uint8_t>(v | (b << i));
    }
    return v;
}

bool EspDs18b20::begin()
{
    gpio_config_t gc = {};
    gc.pin_bit_mask = 1ULL << cfg_.pin;
    gc.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    gc.pull_up_en = cfg_.internal_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    gpio_config(&gc);
    release(cfg_.pin);
    return reset();
}

void EspDs18b20::update(uint32_t now)
{
    switch(state_) {
        case State::Idle:
            if(started_ && now - last_start_ < cfg_.period_ms) return;
            started_ = true;
            last_start_ = now;
            if(!reset()) {
                if(++errors_ >= 3) reading_.valid = false;
                return;
            }
            writeByte(0xCC);   // SKIP ROM
            writeByte(0x44);   // CONVERT T
            state_ = State::Converting;
            return;
        case State::Converting: {
            if(now - last_start_ < 760) return;   // 12-bit conversion: 750 ms
            state_ = State::Idle;
            uint8_t sp[9];
            if(!reset()) {
                if(++errors_ >= 3) reading_.valid = false;
                return;
            }
            writeByte(0xCC);
            writeByte(0xBE);   // READ SCRATCHPAD
            for(int i = 0; i < 9; i++) sp[i] = readByte();
            const int16_t raw = static_cast<int16_t>(sp[0] | (sp[1] << 8));
            /* CRC, plus the 85.0 C power-on value that a missed conversion leaves behind */
            if(crc8(sp, 8) != sp[8] || raw == 0x0550) {
                if(++errors_ >= 3) reading_.valid = false;
                return;
            }
            errors_ = 0;
            reading_.celsius = static_cast<float>(raw) / 16.0f + cfg_.offset_c;
            reading_.valid = true;
            reading_.at_ms = now;
            return;
        }
    }
}

}  // namespace brew::drivers
