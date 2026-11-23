/**
 * I2C master bus (driver/i2c_master). ESP-IDF APIs only (no Arduino).
 */
#pragma once

#include "brew/drivers/interfaces/io.h"

#include <cstddef>
#include <cstdint>

namespace brew::drivers {

class EspI2cBus final : public hal::I2cBus {
public:
    struct Config {
        int      port = 0;
        int      sda = -1;
        int      scl = -1;
        uint32_t hz = 100000;          // PCF8574 is a 100 kHz part
        bool     internal_pullup = true;
        int      timeout_ms = 20;
    };

    explicit EspI2cBus(const Config & cfg) : cfg_(cfg) {}

    bool begin();
    /** Logs every address that ACKs. */
    void scan();

    bool write(uint8_t addr, const uint8_t * d, size_t n) override;
    bool writeRead(uint8_t addr, const uint8_t * w, size_t wn, uint8_t * r, size_t rn) override;
    bool read(uint8_t addr, uint8_t * r, size_t rn) override;
    bool probe(uint8_t addr) override;
    void lock() override;
    void unlock() override;

private:
    void * dev(uint8_t addr);

    Config cfg_;
    void * bus_ = nullptr;    // i2c_master_bus_handle_t
    void * mutex_ = nullptr;  // SemaphoreHandle_t (recursive)
    static constexpr int kMaxDevs = 8;
    uint8_t addrs_[kMaxDevs] = {};
    void * devs_[kMaxDevs] = {};
    int ndevs_ = 0;
};

}  // namespace brew::drivers
