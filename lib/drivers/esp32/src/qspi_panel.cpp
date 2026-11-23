#include "brew/drivers/esp32/qspi_panel.h"
#include "brew/drivers/esp32/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

#include <cstdio>
#include <cstring>

namespace brew::drivers {

namespace {
const char * TAG = "brew.panel";
}  // namespace

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
