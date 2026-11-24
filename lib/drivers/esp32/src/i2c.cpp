#include "brew/drivers/esp32/i2c.h"
#include "brew/drivers/esp32/log.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstdio>
#include <cstring>

namespace brew::drivers {

namespace {
const char * TAG = "brew.i2c";
}  // namespace

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
        BREW_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
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
    BREW_LOGI(TAG, "%s", line);
}

}  // namespace brew::drivers
