/**
 * Waveshare ESP32-S3 Knob Touch LCD 1.8" (360x360 ST77916 over QSPI, CST816 touch, knob).
 *
 * !! UNVERIFIED: never run on hardware. Every pin below is a placeholder (-1), and the ST77916
 * needs its vendor init sequence. Copy both from Waveshare's ESP-IDF demo for this board.
 * The drivers themselves (EspQspiPanel, Cst8xxTouch, EspPcntEncoder) are generic.
 *
 * Runs the controller on the same board with the demo plant and no relays; add outputs the
 * same way boards/crowpanel21 does.
 */
#include <Arduino.h>

#include "brew/app/runtime.h"
#include "brew/controller.h"
#include "brew/drivers/esp32/i2c.h"
#include "brew/drivers/esp32/lcd_init.h"
#include "brew/drivers/esp32/ledc_backlight.h"
#include "brew/drivers/esp32/nvs_settings.h"
#include "brew/drivers/esp32/pcnt_encoder.h"
#include "brew/drivers/esp32/qspi_panel.h"
#include "brew/drivers/esp32/task.h"
#include "brew/drivers/chips/cst8xx.h"
#include "brew/fake_plant.h"
#include "brew/local_bus.h"

/* ---------------- pins: TODO, from Waveshare's demo ---------------- */
#define PIN_LCD_CS   (-1)
#define PIN_LCD_CLK  (-1)
#define PIN_LCD_D0   (-1)
#define PIN_LCD_D1   (-1)
#define PIN_LCD_D2   (-1)
#define PIN_LCD_D3   (-1)
#define PIN_LCD_RST  (-1)
#define PIN_LCD_BL   (-1)
#define PIN_TP_SDA   (-1)
#define PIN_TP_SCL   (-1)
#define PIN_KNOB_A   (-1)
#define PIN_KNOB_B   (-1)

using namespace brew;
using namespace brew::drivers;

namespace {

/* TODO: replace with the vendor sequence from Waveshare's demo; this is only the generic tail. */
const LcdInitCmd kSt77916Init[] = {
    { 0x11, 0, { 0 }, 120 },     // sleep out
    { 0x3A, 1, { 0x55 }, 0 },    // RGB565
    { 0x29, 0, { 0 }, 20 },      // display on
};

EspQspiPanel panel({ .width = 360, .height = 360, .spi_host = 1, .cs = PIN_LCD_CS, .clk = PIN_LCD_CLK,
                     .d0 = PIN_LCD_D0, .d1 = PIN_LCD_D1, .d2 = PIN_LCD_D2, .d3 = PIN_LCD_D3,
                     .rst = PIN_LCD_RST, .pclk_hz = 40000000, .init = kSt77916Init,
                     .init_len = sizeof(kSt77916Init) / sizeof(kSt77916Init[0]), .draw_lines = 40 });
EspLedcBacklight backlight({ .pin = PIN_LCD_BL });
EspI2cBus i2c({ .port = 0, .sda = PIN_TP_SDA, .scl = PIN_TP_SCL, .hz = 400000 });
Cst8xxTouch touch(i2c, { .addr = 0x15, .width = 360, .height = 360 });
EspPcntEncoder encoder({ .pin_a = PIN_KNOB_A, .pin_b = PIN_KNOB_B });

LocalBus bus;
NvsSettingsStore settings("brewdial");
FakePlant plant({});
HysteresisLaw law(ChamberDefaults::kHeatDiffF * 5.0f / 9.0f, ChamberDefaults::kCoolDiffF * 5.0f / 9.0f);
OutputGuard guard(&plant.heat(), &plant.cool(), ChamberDefaults::guard());
Controller controller(bus, plant, law, guard, &settings, Controller::Config{});

}  // namespace

void setup()
{
    Serial.begin(115200);
    i2c.begin();
    settings.begin();
    startPeriodic("ctl", [](void * c, uint32_t now) { static_cast<Controller *>(c)->tick(now); }, &controller, 20);

    app::DisplayConfig d;
    d.panel = &panel;
    d.backlight = &backlight;
    d.touch = &touch;
    d.encoder = &encoder;
    app::startDisplay(d, bus);
    Serial.println("BrewDial ready (Waveshare knob 1.8, UNVERIFIED port)");
}

void loop()
{
    vTaskDelete(nullptr);
}
