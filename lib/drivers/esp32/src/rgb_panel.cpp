#include "brew/drivers/esp32/rgb_panel.h"
#include "brew/drivers/esp32/log.h"
#include "brew/drivers/esp32/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#if SOC_LCD_RGB_SUPPORTED
#include "esp_lcd_panel_rgb.h"
#endif

#include <cstdio>
#include <cstring>

namespace brew::drivers {

namespace {
const char * TAG = "brew.panel";
}  // namespace

void EspRgbPanel::spi9(bool dc, uint8_t v)
{
    const auto sck = static_cast<gpio_num_t>(cfg_.spi_sck);
    const auto sda = static_cast<gpio_num_t>(cfg_.spi_sda);
    /* 9-bit word: D/C bit first, then the byte MSB first. Data latched on SCK rising. */
    gpio_set_level(sda, dc);
    gpio_set_level(sck, 1);
    gpio_set_level(sck, 0);
    for(uint8_t bit = 0x80; bit; bit >>= 1) {
        gpio_set_level(sda, (v & bit) != 0);
        gpio_set_level(sck, 1);
        gpio_set_level(sck, 0);
    }
}

void EspRgbPanel::spiCmd(uint8_t cmd, const uint8_t * data, size_t n)
{
    spi9(false, cmd);
    for(size_t i = 0; i < n; i++) spi9(true, data[i]);
}

bool EspRgbPanel::begin()
{
#if SOC_LCD_RGB_SUPPORTED
    /* ---- controller init over the 3-wire bus (CS held low across the sequence and raised for
     * delays, the same framing as Arduino_GFX's batch operations) ---- */
    const auto cs = static_cast<gpio_num_t>(cfg_.spi_cs);
    gpio_config_t gc = {};
    gc.pin_bit_mask = (1ULL << cfg_.spi_cs) | (1ULL << cfg_.spi_sck) | (1ULL << cfg_.spi_sda);
    gc.mode = GPIO_MODE_OUTPUT;
    gpio_config(&gc);
    gpio_set_level(cs, 1);
    gpio_set_level(static_cast<gpio_num_t>(cfg_.spi_sck), 0);
    gpio_set_level(static_cast<gpio_num_t>(cfg_.spi_sda), 0);

    gpio_set_level(cs, 0);
    spiCmd(0x01, nullptr, 0);   // software reset (the hardware reset line is board-specific)
    gpio_set_level(cs, 1);
    delayMs(120);

    gpio_set_level(cs, 0);
    for(size_t i = 0; i < cfg_.init_len; i++) {
        const LcdInitCmd & c = cfg_.init[i];
        spiCmd(c.cmd, c.data, c.len);
        if(c.delay_ms) {
            gpio_set_level(cs, 1);
            delayMs(c.delay_ms);
            gpio_set_level(cs, 0);
        }
    }
    gpio_set_level(cs, 1);

    /* ---- RGB scan-out ---- */
    esp_lcd_rgb_panel_config_t pc = {};
    pc.clk_src = LCD_CLK_SRC_DEFAULT;
    pc.timings.pclk_hz = cfg_.pclk_hz;
    pc.timings.h_res = cfg_.width;
    pc.timings.v_res = cfg_.height;
    pc.timings.hsync_pulse_width = cfg_.hsync_pulse;
    pc.timings.hsync_back_porch = cfg_.hsync_back;
    pc.timings.hsync_front_porch = cfg_.hsync_front;
    pc.timings.vsync_pulse_width = cfg_.vsync_pulse;
    pc.timings.vsync_back_porch = cfg_.vsync_back;
    pc.timings.vsync_front_porch = cfg_.vsync_front;
    pc.timings.flags.hsync_idle_low = cfg_.hsync_idle_low;
    pc.timings.flags.vsync_idle_low = cfg_.vsync_idle_low;
    pc.timings.flags.de_idle_high = cfg_.de_idle_high;
    pc.timings.flags.pclk_active_neg = cfg_.pclk_active_neg;
    pc.timings.flags.pclk_idle_high = cfg_.pclk_idle_high;
    pc.data_width = 16;
    pc.bits_per_pixel = 16;
    pc.num_fbs = 1;
    pc.bounce_buffer_size_px = cfg_.bounce_buffer_px;
    pc.dma_burst_size = 64;
    pc.hsync_gpio_num = cfg_.hsync;
    pc.vsync_gpio_num = cfg_.vsync;
    pc.de_gpio_num = cfg_.de;
    pc.pclk_gpio_num = cfg_.pclk;
    pc.disp_gpio_num = -1;
    /* data[0..4] carry the pixel's low 5 bits (LVGL blue), data[11..15] the high 5 (red). */
    const int * lo = cfg_.swap_rb ? cfg_.r : cfg_.b;
    const int * hi = cfg_.swap_rb ? cfg_.b : cfg_.r;
    for(int i = 0; i < 5; i++) pc.data_gpio_nums[i] = lo[i];
    for(int i = 0; i < 6; i++) pc.data_gpio_nums[5 + i] = cfg_.g[i];
    for(int i = 0; i < 5; i++) pc.data_gpio_nums[11 + i] = hi[i];
    pc.flags.fb_in_psram = 1;

    esp_lcd_panel_handle_t p = nullptr;
    esp_err_t err = esp_lcd_new_rgb_panel(&pc, &p);
    if(err != ESP_OK) {
        BREW_LOGE(TAG, "esp_lcd_new_rgb_panel: %s", esp_err_to_name(err));
        return false;
    }
    esp_lcd_panel_reset(p);
    esp_lcd_panel_init(p);
    panel_ = p;

    /* clear to black */
    const int lines = 8;
    auto * z = static_cast<uint16_t *>(heap_caps_calloc(static_cast<size_t>(cfg_.width) * lines, 2, MALLOC_CAP_INTERNAL));
    if(z) {
        for(int y = 0; y < cfg_.height; y += lines) {
            const int y2 = y + lines > cfg_.height ? cfg_.height : y + lines;
            esp_lcd_panel_draw_bitmap(p, 0, y, cfg_.width, y2, z);
        }
        heap_caps_free(z);
    }
    return true;
#else
    BREW_LOGE(TAG, "this chip has no RGB LCD peripheral");
    return false;
#endif
}

void EspRgbPanel::flush(const hal::Area & a, uint16_t * px, FlushDone done, void * ctx)
{
    /* Copies the band into the PSRAM framebuffer (with cache write-back) and returns. */
    if(panel_)
        esp_lcd_panel_draw_bitmap(static_cast<esp_lcd_panel_handle_t>(panel_), a.x1, a.y1, a.x2 + 1, a.y2 + 1, px);
    done(ctx);
}

uint32_t EspRgbPanel::drawBufferCaps() const
{
    return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
}

}  // namespace brew::drivers
