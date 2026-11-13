/* I2C bus, GPIO, PCNT encoder, LEDC backlight, UART, NVS. */
#include "brew/drivers/esp32.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <cstdio>
#include <cstring>

namespace brew::drivers {

namespace {
const char * TAG = "brew.drv";
}

void delayMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

/* ------------------------------------ I2C ------------------------------------ */

bool EspI2cBus::begin()
{
    mutex_ = xSemaphoreCreateRecursiveMutex();
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = static_cast<i2c_port_num_t>(cfg_.port);
    bc.sda_io_num = static_cast<gpio_num_t>(cfg_.sda);
    bc.scl_io_num = static_cast<gpio_num_t>(cfg_.scl);
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = cfg_.internal_pullup;
    i2c_master_bus_handle_t h = nullptr;
    const esp_err_t err = i2c_new_master_bus(&bc, &h);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return false;
    }
    bus_ = h;
    return true;
}

void EspI2cBus::lock()
{
    if(mutex_) xSemaphoreTakeRecursive(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
}

void EspI2cBus::unlock()
{
    if(mutex_) xSemaphoreGiveRecursive(static_cast<SemaphoreHandle_t>(mutex_));
}

void * EspI2cBus::dev(uint8_t addr)
{
    for(int i = 0; i < ndevs_; i++)
        if(addrs_[i] == addr) return devs_[i];
    if(!bus_ || ndevs_ == kMaxDevs) return nullptr;
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = addr;
    dc.scl_speed_hz = cfg_.hz;
    i2c_master_dev_handle_t d = nullptr;
    if(i2c_master_bus_add_device(static_cast<i2c_master_bus_handle_t>(bus_), &dc, &d) != ESP_OK) return nullptr;
    addrs_[ndevs_] = addr;
    devs_[ndevs_++] = d;
    return d;
}

bool EspI2cBus::write(uint8_t addr, const uint8_t * d, size_t n)
{
    hal::I2cLock l(*this);
    auto * h = static_cast<i2c_master_dev_handle_t>(dev(addr));
    return h && i2c_master_transmit(h, d, n, cfg_.timeout_ms) == ESP_OK;
}

bool EspI2cBus::writeRead(uint8_t addr, const uint8_t * w, size_t wn, uint8_t * r, size_t rn)
{
    hal::I2cLock l(*this);
    auto * h = static_cast<i2c_master_dev_handle_t>(dev(addr));
    return h && i2c_master_transmit_receive(h, w, wn, r, rn, cfg_.timeout_ms) == ESP_OK;
}

bool EspI2cBus::read(uint8_t addr, uint8_t * r, size_t rn)
{
    hal::I2cLock l(*this);
    auto * h = static_cast<i2c_master_dev_handle_t>(dev(addr));
    return h && i2c_master_receive(h, r, rn, cfg_.timeout_ms) == ESP_OK;
}

bool EspI2cBus::probe(uint8_t addr)
{
    hal::I2cLock l(*this);
    return bus_ && i2c_master_probe(static_cast<i2c_master_bus_handle_t>(bus_), addr, cfg_.timeout_ms) == ESP_OK;
}

void EspI2cBus::scan()
{
    char line[160];
    int o = std::snprintf(line, sizeof(line), "I2C%d scan:", cfg_.port);
    for(uint8_t a = 0x08; a < 0x78; a++)
        if(probe(a) && o < static_cast<int>(sizeof(line)) - 6) o += std::snprintf(line + o, sizeof(line) - o, " 0x%02X", a);
    ESP_LOGI(TAG, "%s", line);
}

/* ------------------------------------ GPIO ----------------------------------- */

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

/* ---------------------------------- Encoder ---------------------------------- */

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

/* --------------------------------- Backlight --------------------------------- */

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

/* ----------------------------------- UART ------------------------------------ */

bool EspUartStream::begin()
{
    const auto port = static_cast<uart_port_t>(cfg_.port);
    uart_config_t c = {};
    c.baud_rate = static_cast<int>(cfg_.baud);
    c.data_bits = UART_DATA_8_BITS;
    c.parity = UART_PARITY_DISABLE;
    c.stop_bits = UART_STOP_BITS_1;
    c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    c.source_clk = UART_SCLK_DEFAULT;
    if(uart_driver_install(port, static_cast<int>(cfg_.rx_buffer), static_cast<int>(cfg_.rx_buffer), 0, nullptr, 0) != ESP_OK)
        return false;
    if(uart_param_config(port, &c) != ESP_OK) return false;
    if(uart_set_pin(port, cfg_.tx, cfg_.rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return false;
    ready_ = true;
    return true;
}

size_t EspUartStream::write(const uint8_t * d, size_t n)
{
    if(!ready_) return 0;
    const int w = uart_write_bytes(static_cast<uart_port_t>(cfg_.port), d, n);
    return w < 0 ? 0 : static_cast<size_t>(w);
}

size_t EspUartStream::read(uint8_t * buf, size_t max)
{
    if(!ready_) return 0;
    const int r = uart_read_bytes(static_cast<uart_port_t>(cfg_.port), buf, static_cast<uint32_t>(max), 0);
    return r < 0 ? 0 : static_cast<size_t>(r);
}

/* ---------------------------------- Storage ---------------------------------- */

bool NvsSettingsStore::begin()
{
    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    nvs_handle_t h = 0;
    if(nvs_open(ns_, NVS_READWRITE, &h) != ESP_OK) return false;
    h_ = h;
    open_ = true;
    return true;
}

bool NvsSettingsStore::getI32(const char * key, int32_t & out)
{
    return open_ && nvs_get_i32(h_, key, &out) == ESP_OK;
}

bool NvsSettingsStore::setI32(const char * key, int32_t v)
{
    int32_t cur;
    if(open_ && nvs_get_i32(h_, key, &cur) == ESP_OK && cur == v) return true;   // spare the flash
    return open_ && nvs_set_i32(h_, key, v) == ESP_OK;
}

bool NvsSettingsStore::getStr(const char * key, char * out, size_t max)
{
    size_t n = max;
    return open_ && nvs_get_str(h_, key, out, &n) == ESP_OK;
}

bool NvsSettingsStore::setStr(const char * key, const char * v)
{
    char cur[64];
    size_t n = sizeof(cur);
    if(open_ && nvs_get_str(h_, key, cur, &n) == ESP_OK && std::strcmp(cur, v) == 0) return true;
    return open_ && nvs_set_str(h_, key, v) == ESP_OK;
}

bool NvsSettingsStore::commit()
{
    return open_ && nvs_commit(h_) == ESP_OK;
}

}  // namespace brew::drivers
