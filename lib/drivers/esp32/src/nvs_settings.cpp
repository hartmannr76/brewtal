#include "brew/drivers/esp32/nvs_settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <cstdio>
#include <cstring>

namespace brew::drivers {

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
