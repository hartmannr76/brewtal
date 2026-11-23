/**
 * 16-bit parallel RGB panel (esp_lcd_panel_rgb) with a 3-wire SPI init bus. ESP-IDF APIs only (no Arduino).
 */
#pragma once

#include "brew/drivers/interfaces/display.h"
#include "brew/drivers/esp32/lcd_init.h"

#include <cstddef>
#include <cstdint>

namespace brew::drivers {

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

}  // namespace brew::drivers
