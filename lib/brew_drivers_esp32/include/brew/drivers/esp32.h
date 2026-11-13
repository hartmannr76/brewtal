/**
 * ESP32 peripheral drivers, written against ESP-IDF APIs only (no Arduino), so they build
 * under PlatformIO's Arduino-ESP32 3.x and under plain ESP-IDF.
 */
#pragma once

#include "brew/hal/display.h"
#include "brew/hal/input.h"
#include "brew/hal/io.h"
#include "brew/hal/platform.h"

#include <cstddef>
#include <cstdint>

namespace brew::drivers {

/* ------------------------------------ I2C ------------------------------------ */

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

/* ------------------------------------ GPIO ----------------------------------- */

class EspGpioOut final : public hal::DigitalOut {
public:
    explicit EspGpioOut(int pin, bool initial = false) : pin_(pin), initial_(initial) {}
    bool begin();
    void write(bool high) override;

private:
    int pin_;
    bool initial_;
};

class EspGpioIn final : public hal::DigitalIn {
public:
    explicit EspGpioIn(int pin, bool pullup = true) : pin_(pin), pullup_(pullup) {}
    bool begin();
    bool read() override;

private:
    int pin_;
    bool pullup_;
};

/* ---------------------------------- Encoder ---------------------------------- */

/**
 * Quadrature encoder decoded x4 in hardware by the PCNT peripheral, with a glitch filter
 * and the internal pull-ups on. count() increases clockwise for the usual A/B wiring;
 * set `reverse` if yours counts the other way.
 */
class EspPcntEncoder final : public hal::Encoder {
public:
    struct Config {
        int      pin_a = -1;
        int      pin_b = -1;
        uint32_t glitch_ns = 1000;
        bool     reverse = false;
    };

    explicit EspPcntEncoder(const Config & cfg) : cfg_(cfg) {}
    bool begin() override;
    int32_t count() override;

private:
    Config cfg_;
    void * unit_ = nullptr;   // pcnt_unit_handle_t
};

/* --------------------------------- Backlight --------------------------------- */

class EspLedcBacklight final : public hal::Backlight {
public:
    struct Config {
        int      pin = -1;
        uint32_t freq_hz = 5000;
        int      timer = 0;      // LEDC timer / channel to claim
        int      channel = 0;
        bool     active_low = false;
    };

    explicit EspLedcBacklight(const Config & cfg) : cfg_(cfg) {}
    bool begin();
    void set(uint8_t level) override;

private:
    Config cfg_;
    bool ready_ = false;
};

/* ---------------------------------- Panels ----------------------------------- */

/** One entry of a panel init sequence. */
struct LcdInitCmd {
    uint8_t  cmd;
    uint8_t  len;
    uint8_t  data[16];
    uint16_t delay_ms;
};

/** ST7701S "type 5" sequence (from Arduino_GFX), used by the Elecrow CrowPanel 2.1". */
extern const LcdInitCmd kSt7701Type5Init[];
extern const size_t kSt7701Type5InitLen;

/**
 * 16-bit parallel RGB panel (esp_lcd_panel_rgb, ESP32-S3) whose controller is configured over a
 * 3-wire 9-bit SPI bus (ST7701 and friends). The framebuffer lives in PSRAM; flush() copies a
 * band into it and returns immediately.
 */
class EspRgbPanel final : public hal::DisplayPanel {
public:
    struct Config {
        uint16_t width = 480, height = 480;
        /* 3-wire init bus */
        int spi_cs = -1, spi_sck = -1, spi_sda = -1;
        const LcdInitCmd * init = nullptr;
        size_t init_len = 0;
        /* RGB bus */
        int de = -1, vsync = -1, hsync = -1, pclk = -1;
        int r[5] = { -1, -1, -1, -1, -1 };
        int g[6] = { -1, -1, -1, -1, -1, -1 };
        int b[5] = { -1, -1, -1, -1, -1 };
        uint32_t pclk_hz = 12000000;
        uint16_t hsync_pulse = 4, hsync_back = 20, hsync_front = 10;
        uint16_t vsync_pulse = 4, vsync_back = 20, vsync_front = 10;
        bool hsync_idle_low = false, vsync_idle_low = false;
        bool de_idle_high = false, pclk_active_neg = false, pclk_idle_high = false;
        /* The panel's R and B are wired the other way round: swap the 5-bit groups on the bus
         * (free: done by pin assignment instead of a per-pixel loop). */
        bool swap_rb = false;
        size_t bounce_buffer_px = 480 * 20;
        uint16_t draw_lines = 48;
    };

    explicit EspRgbPanel(const Config & cfg) : cfg_(cfg) {}

    bool begin() override;
    uint16_t width() const override { return cfg_.width; }
    uint16_t height() const override { return cfg_.height; }
    void flush(const hal::Area & a, uint16_t * px, FlushDone done, void * ctx) override;
    uint16_t drawBufferLines() const override { return cfg_.draw_lines; }
    uint32_t drawBufferCaps() const override;

private:
    void spiCmd(uint8_t cmd, const uint8_t * data, size_t n);
    void spi9(bool dc, uint8_t v);

    Config cfg_;
    void * panel_ = nullptr;   // esp_lcd_panel_handle_t
};

/**
 * QSPI panel through esp_lcd's SPI IO in quad mode (ST77916, SH8601, CO5300 style:
 * 32-bit command words, 0x02 for parameter writes, 0x32 for quad pixel writes).
 * Pixels are byte-swapped to big-endian and sent by DMA; done() fires from the DMA callback.
 */
class EspQspiPanel final : public hal::DisplayPanel {
public:
    struct Config {
        uint16_t width = 360, height = 360;
        int spi_host = 1;        // SPI2_HOST
        int cs = -1, clk = -1, d0 = -1, d1 = -1, d2 = -1, d3 = -1, rst = -1;
        uint32_t pclk_hz = 40000000;
        const LcdInitCmd * init = nullptr;
        size_t init_len = 0;
        uint16_t draw_lines = 40;
    };

    explicit EspQspiPanel(const Config & cfg) : cfg_(cfg) {}

    bool begin() override;
    uint16_t width() const override { return cfg_.width; }
    uint16_t height() const override { return cfg_.height; }
    void flush(const hal::Area & a, uint16_t * px, FlushDone done, void * ctx) override;
    uint16_t drawBufferLines() const override { return cfg_.draw_lines; }
    uint32_t drawBufferCaps() const override;

    /* called from the esp_lcd DMA-done ISR */
    void transferDone();

private:
    void param(uint8_t cmd, const uint8_t * data, size_t n);

    Config cfg_;
    void * io_ = nullptr;   // esp_lcd_panel_io_handle_t
    FlushDone done_ = nullptr;
    void * done_ctx_ = nullptr;
};

/* ----------------------------------- UART ------------------------------------ */

class EspUartStream final : public hal::ByteStream {
public:
    struct Config {
        int      port = 1;
        int      tx = -1, rx = -1;
        uint32_t baud = 115200;
        size_t   rx_buffer = 1024;
    };

    explicit EspUartStream(const Config & cfg) : cfg_(cfg) {}
    bool begin() override;
    size_t write(const uint8_t * d, size_t n) override;
    size_t read(uint8_t * buf, size_t max) override;

private:
    Config cfg_;
    bool ready_ = false;
};

/* ---------------------------------- Storage ---------------------------------- */

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

/** Blocking delay usable from board bring-up code. */
void delayMs(uint32_t ms);

}  // namespace brew::drivers
