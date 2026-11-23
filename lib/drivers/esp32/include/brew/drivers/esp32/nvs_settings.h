/**
 * Settings in NVS flash as typed key/value pairs. ESP-IDF APIs only (no Arduino).
 */
#pragma once

#include "brew/drivers/interfaces/platform.h"

#include <cstddef>
#include <cstdint>

namespace brew::drivers {

class NvsSettingsStore final : public hal::SettingsStore {
public:
    explicit NvsSettingsStore(const char * ns) : ns_(ns) {}
    bool begin();
    bool getI32(const char * key, int32_t & out) override;
    bool setI32(const char * key, int32_t v) override;
    bool getStr(const char * key, char * out, size_t max) override;
    bool setStr(const char * key, const char * v) override;
    bool commit() override;

private:
    const char * ns_;
    uint32_t h_ = 0;
    bool open_ = false;
};

}  // namespace brew::drivers
