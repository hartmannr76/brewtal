/* Display panels on esp_lcd: 16-bit RGB (+ 3-wire SPI init) and QSPI. */
#include "brew/drivers/esp32.h"

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

#include <cstring>

namespace brew::drivers {

namespace {
const char * TAG = "brew.panel";
}

/* ST7701S "type 5" init sequence, transcribed from Arduino_GFX's st7701_type5_init_operations
 * (the one Elecrow's CrowPanel 2.1" factory sketch uses). */
const LcdInitCmd kSt7701Type5Init[] = {
    { 0xFF, 5, { 0x77, 0x01, 0x00, 0x00, 0x10 }, 0 },
    { 0xC0, 2, { 0x3B, 0x00 }, 0 },
    { 0xC1, 2, { 0x0B, 0x02 }, 0 },   // VBP
    { 0xC2, 2, { 0x00, 0x02 }, 0 },
    { 0xCC, 1, { 0x10 }, 0 },
    { 0xCD, 1, { 0x08 }, 0 },
    { 0xB0, 16, { 0x02, 0x13, 0x1B, 0x0D, 0x10, 0x05, 0x08, 0x07, 0x07, 0x24, 0x04, 0x11, 0x0E, 0x2C, 0x33, 0x1D }, 0 },   // +gamma
    { 0xB1, 16, { 0x05, 0x13, 0x1B, 0x0D, 0x11, 0x05, 0x08, 0x07, 0x07, 0x24, 0x04, 0x11, 0x0E, 0x2C, 0x33, 0x1D }, 0 },   // -gamma
    { 0xFF, 5, { 0x77, 0x01, 0x00, 0x00, 0x11 }, 0 },
    { 0xB0, 1, { 0x5D }, 0 },
    { 0xB1, 1, { 0x43 }, 0 },   // VCOM amplitude
    { 0xB2, 1, { 0x81 }, 0 },   // VGH 12 V
    { 0xB3, 1, { 0x80 }, 0 },
    { 0xB5, 1, { 0x43 }, 0 },   // VGL -8.3 V
    { 0xB7, 1, { 0x85 }, 0 },
    { 0xB8, 1, { 0x20 }, 0 },
    { 0xC1, 1, { 0x78 }, 0 },
    { 0xC2, 1, { 0x78 }, 0 },
    { 0xD0, 1, { 0x88 }, 0 },
    { 0xE0, 3, { 0x00, 0x00, 0x02 }, 0 },
    { 0xE1, 11, { 0x03, 0xA0, 0x00, 0x00, 0x04, 0xA0, 0x00, 0x00, 0x00, 0x20, 0x20 }, 0 },
    { 0xE2, 13, { 0 }, 0 },
    { 0xE3, 4, { 0x00, 0x00, 0x11, 0x00 }, 0 },
    { 0xE4, 2, { 0x22, 0x00 }, 0 },
    { 0xE5, 16, { 0x05, 0xEC, 0xA0, 0xA0, 0x07, 0xEE, 0xA0, 0xA0, 0, 0, 0, 0, 0, 0, 0, 0 }, 0 },
    { 0xE6, 4, { 0x00, 0x00, 0x11, 0x00 }, 0 },
    { 0xE7, 2, { 0x22, 0x00 }, 0 },
    { 0xE8, 16, { 0x06, 0xED, 0xA0, 0xA0, 0x08, 0xEF, 0xA0, 0xA0, 0, 0, 0, 0, 0, 0, 0, 0 }, 0 },
    { 0xEB, 7, { 0x00, 0x00, 0x40, 0x40, 0x00, 0x00, 0x00 }, 0 },
    { 0xED, 16, { 0xFF, 0xFF, 0xFF, 0xBA, 0x0A, 0xBF, 0x45, 0xFF, 0xFF, 0x54, 0xFB, 0xA0, 0xAB, 0xFF, 0xFF, 0xFF }, 0 },
    { 0xEF, 6, { 0x10, 0x0D, 0x04, 0x08, 0x3F, 0x1F }, 0 },
    { 0xFF, 5, { 0x77, 0x01, 0x00, 0x00, 0x13 }, 0 },
    { 0xEF, 1, { 0x08 }, 0 },
    { 0xFF, 5, { 0x77, 0x01, 0x00, 0x00, 0x00 }, 0 },
    { 0x36, 1, { 0x08 }, 0 },   // MADCTL: BGR
    { 0x3A, 1, { 0x60 }, 0 },   // RGB666 interface
    { 0x11, 0, { 0 }, 100 },    // sleep out
    { 0x29, 0, { 0 }, 50 },     // display on
};
const size_t kSt7701Type5InitLen = sizeof(kSt7701Type5Init) / sizeof(kSt7701Type5Init[0]);

/* ------------------------------- EspRgbPanel ------------------------------- */

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
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel: %s", esp_err_to_name(err));
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
    ESP_LOGE(TAG, "this chip has no RGB LCD peripheral");
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

/* ------------------------------- EspQspiPanel ------------------------------- */

namespace {
bool qspi_done_cb(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void * ctx)
{
    static_cast<EspQspiPanel *>(ctx)->transferDone();
    return false;
}

constexpr uint32_t kQspiWriteParam = 0x02;
constexpr uint32_t kQspiWritePixels = 0x32;
}  // namespace

void EspQspiPanel::transferDone()
{
    FlushDone d = done_;
    done_ = nullptr;
    if(d) d(done_ctx_);
}

void EspQspiPanel::param(uint8_t cmd, const uint8_t * data, size_t n)
{
    esp_lcd_panel_io_tx_param(static_cast<esp_lcd_panel_io_handle_t>(io_),
                              static_cast<int>((kQspiWriteParam << 24) | (static_cast<uint32_t>(cmd) << 8)), data, n);
}

bool EspQspiPanel::begin()
{
    spi_bus_config_t bc = {};
    bc.sclk_io_num = cfg_.clk;
    bc.data0_io_num = cfg_.d0;
    bc.data1_io_num = cfg_.d1;
    bc.data2_io_num = cfg_.d2;
    bc.data3_io_num = cfg_.d3;
    bc.max_transfer_sz = static_cast<int>(cfg_.width) * cfg_.draw_lines * 2;
    bc.flags = SPICOMMON_BUSFLAG_QUAD;
    const auto host = static_cast<spi_host_device_t>(cfg_.spi_host);
    esp_err_t err = spi_bus_initialize(host, &bc, SPI_DMA_CH_AUTO);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_io_spi_config_t ioc = {};
    ioc.cs_gpio_num = cfg_.cs;
    ioc.dc_gpio_num = -1;
    ioc.spi_mode = 0;
    ioc.pclk_hz = cfg_.pclk_hz;
    ioc.trans_queue_depth = 10;
    ioc.on_color_trans_done = qspi_done_cb;
    ioc.user_ctx = this;
    ioc.lcd_cmd_bits = 32;
    ioc.lcd_param_bits = 8;
    ioc.flags.quad_mode = 1;
    esp_lcd_panel_io_handle_t io = nullptr;
    err = esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(host), &ioc, &io);
    if(err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi: %s", esp_err_to_name(err));
        return false;
    }
    io_ = io;

    if(cfg_.rst >= 0) {
        gpio_config_t gc = {};
        gc.pin_bit_mask = 1ULL << cfg_.rst;
        gc.mode = GPIO_MODE_OUTPUT;
        gpio_config(&gc);
        gpio_set_level(static_cast<gpio_num_t>(cfg_.rst), 0);
        delayMs(10);
        gpio_set_level(static_cast<gpio_num_t>(cfg_.rst), 1);
        delayMs(120);
    }
    for(size_t i = 0; i < cfg_.init_len; i++) {
        const LcdInitCmd & c = cfg_.init[i];
        param(c.cmd, c.len ? c.data : nullptr, c.len);
        if(c.delay_ms) delayMs(c.delay_ms);
    }
    return true;
}

void EspQspiPanel::flush(const hal::Area & a, uint16_t * px, FlushDone done, void * ctx)
{
    const size_t n = static_cast<size_t>(a.x2 - a.x1 + 1) * static_cast<size_t>(a.y2 - a.y1 + 1);
    for(size_t i = 0; i < n; i++) px[i] = static_cast<uint16_t>((px[i] << 8) | (px[i] >> 8));   // big-endian on the wire

    const uint8_t caset[4] = { static_cast<uint8_t>(a.x1 >> 8), static_cast<uint8_t>(a.x1),
                               static_cast<uint8_t>(a.x2 >> 8), static_cast<uint8_t>(a.x2) };
    const uint8_t raset[4] = { static_cast<uint8_t>(a.y1 >> 8), static_cast<uint8_t>(a.y1),
                               static_cast<uint8_t>(a.y2 >> 8), static_cast<uint8_t>(a.y2) };
    param(0x2A, caset, 4);
    param(0x2B, raset, 4);
    done_ = done;
    done_ctx_ = ctx;
    esp_lcd_panel_io_tx_color(static_cast<esp_lcd_panel_io_handle_t>(io_),
                              static_cast<int>((kQspiWritePixels << 24) | (0x2Cu << 8)), px, n * 2);
}

uint32_t EspQspiPanel::drawBufferCaps() const
{
    return MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
}

}  // namespace brew::drivers
