/**
 * QSPI panel through esp_lcd SPI IO in quad mode. ESP-IDF APIs only (no Arduino).
 */
#pragma once

#include "brew/drivers/interfaces/display.h"
#include "brew/drivers/esp32/lcd_init.h"

#include <cstddef>
#include <cstdint>

namespace brew::drivers {

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

}  // namespace brew::drivers
