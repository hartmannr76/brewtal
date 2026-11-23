/**
 * Composition root for the CrowPanel 2.1". Builds the drivers, then hands them to the
 * portable app: brew::Controller (control task) and brew::app::startDisplay (UI + input).
 *
 * Roles (platformio.ini):
 *   crowpanel21          everything on this board (LocalBus between UI and controller)
 *   crowpanel21_display  display only; the controller runs on another board, over UART
 */
#include <Arduino.h>   // only for the setup()/loop() entry points and Serial

#include "board_config.h"

#include "brew/app/runtime.h"
#include "brew/controller.h"
#include "brew/drivers/ds18b20.h"
#include "brew/drivers/esp32.h"
#include "brew/drivers/esp_task.h"
#include "brew/drivers/i2c_chips.h"
#include "brew/fake_plant.h"
#include "brew/link.h"
#include "brew/local_bus.h"
#include "lvgl.h"   // version banner only

#ifndef BREW_ROLE_DISPLAY
#define BREW_ROLE_DISPLAY 0
#endif

using namespace brew;
using namespace brew::drivers;

namespace {

/* ---------------- buses + expanders ---------------- */
EspI2cBus i2c({ .port = 0, .sda = I2C_SDA_PIN, .scl = I2C_SCL_PIN, .hz = 100000 });
Pcf8574 pcf(i2c, PCF8574_ADDR, /* input mask */ 1u << PCF_KNOB_SWITCH);

/* ---------------- display + input ---------------- */
EspRgbPanel::Config panelConfig()
{
    EspRgbPanel::Config c;
    c.width = 480;
    c.height = 480;
    c.spi_cs = 16;
    c.spi_sck = 2;
    c.spi_sda = 1;
    c.init = kSt7701Type5Init;
    c.init_len = kSt7701Type5InitLen;
    c.de = 40;
    c.vsync = 7;
    c.hsync = 15;
    c.pclk = 41;
    const int r[5] = { 46, 3, 8, 18, 17 }, g[6] = { 14, 13, 12, 11, 10, 9 }, b[5] = { 5, 45, 48, 47, 21 };
    for(int i = 0; i < 5; i++) c.r[i] = r[i], c.b[i] = b[i];
    for(int i = 0; i < 6; i++) c.g[i] = g[i];
    c.pclk_hz = 12000000;
    c.hsync_pulse = 4, c.hsync_back = 20, c.hsync_front = 10;
    c.vsync_pulse = 4, c.vsync_back = 20, c.vsync_front = 10;
    c.swap_rb = true;   // this panel's wiring wants R and B swapped (Elecrow's driver did it per pixel)
    c.bounce_buffer_px = 480 * 20;
    c.draw_lines = 48;
    return c;
}
EspRgbPanel panel(panelConfig());
EspLedcBacklight backlight({ .pin = BACKLIGHT_PIN });
Cst8xxTouch touch(i2c, { .addr = TOUCH_ADDR, .width = 480, .height = 480, .x_offset = 0,
                         .y_offset = TOUCH_Y_OFFSET, .swap_xy = false, .mirror_x = false,
                         .mirror_y = false, .nack_means_released = false });
EspPcntEncoder encoder({ .pin_a = ENCODER_A_PIN, .pin_b = ENCODER_B_PIN });

#if !BREW_ROLE_DISPLAY
/* ---------------- controller side ---------------- */
Mcp23008 relays(i2c, MCP23008_ADDR, (1u << MCP_PIN_HEAT) | (1u << MCP_PIN_COOL));
NvsSettingsStore settings("brewdial");
LocalBus bus;

#if BREW_DEMO
FakePlant plant({});
hal::TempSensor & sensor = plant;
hal::DigitalOut & heatOut = plant.heat(&relays.out(MCP_PIN_HEAT));   // LEDs still show the state
hal::DigitalOut & coolOut = plant.cool(&relays.out(MCP_PIN_COOL));
#else
static_assert(PROBE_PIN >= 0, "Set PROBE_PIN in board_config.h (or build with BREW_DEMO=1)");
EspDs18b20 probe({ .pin = PROBE_PIN });
hal::TempSensor & sensor = probe;
hal::DigitalOut & heatOut = relays.out(MCP_PIN_HEAT);
hal::DigitalOut & coolOut = relays.out(MCP_PIN_COOL);
#endif

HysteresisLaw law(ChamberDefaults::kHeatDiffF * 5.0f / 9.0f, ChamberDefaults::kCoolDiffF * 5.0f / 9.0f);   // see ChamberDefaults
OutputGuard guard(&heatOut, &coolOut, ChamberDefaults::guard());   // 3 min restart delay per relay

Controller::Config controllerConfig()
{
    Controller::Config c;
    c.period_ms = 1500;
    c.history_period_s = HISTORY_PERIOD_S;
    c.batch = "Pale ale";
    c.day = 4;
    return c;
}
Controller controller(bus, sensor, law, guard, &settings, controllerConfig());
UiPort & uiPort = bus;
#else
/* ---------------- display-only: controller is on another board ---------------- */
EspUartStream uart({ .port = LINK_UART_PORT, .tx = LINK_UART_TX, .rx = LINK_UART_RX, .baud = LINK_UART_BAUD });
StreamPacketLink uartLink(uart);
RemoteControllerProxy remote(uartLink);
UiPort & uiPort = remote;
#endif

void lcdPowerOffAfterInit()
{
    pcf.write(PCF_LCD_POWER, false);   // as in Elecrow's sketch
}

}  // namespace

void setup()
{
    Serial.begin(115200);
    delay(100);
    Serial.printf("BrewDial (CrowPanel 2.1\"%s): Arduino-ESP32 %s, LVGL %d.%d.%d, PSRAM %u KB\n",
                  BREW_ROLE_DISPLAY ? ", display only" : "", ESP_ARDUINO_VERSION_STR, LVGL_VERSION_MAJOR,
                  LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, static_cast<unsigned>(ESP.getPsramSize() / 1024));
    if(ESP.getPsramSize() == 0) {
        Serial.println("FATAL: no PSRAM. Check board_build.arduino.memory_type = qio_opi");
        for(;;) delay(1000);
    }

    i2c.begin();
    i2c.scan();
    if(!pcf.begin()) Serial.println("PCF8574 not found");

    /* LCD power + reset, touch reset (all on the expander) */
    pcf.write(PCF_LCD_POWER, true);  delayMs(100);
    pcf.write(PCF_LCD_RST, true);    delayMs(100);
    pcf.write(PCF_LCD_RST, false);   delayMs(120);
    pcf.write(PCF_LCD_RST, true);    delayMs(120);
    pcf.write(PCF_TOUCH_RST, true);  delayMs(100);
    pcf.write(PCF_TOUCH_RST, false); delayMs(120);
    pcf.write(PCF_TOUCH_RST, true);  delayMs(120);
    pcf.write(PCF_TOUCH_INT, true);  delayMs(120);

#if !BREW_ROLE_DISPLAY
    settings.begin();
    if(!relays.begin()) Serial.println("MCP23008 not answering yet (outputs retry on every write)");
    startPeriodic("ctl", [](void * c, uint32_t now) { static_cast<Controller *>(c)->tick(now); }, &controller, 20);
#else
    if(!uartLink.begin()) Serial.println("UART link init failed");
#endif

    app::DisplayConfig d;
    d.panel = &panel;
    d.backlight = &backlight;
    d.backlight_level = BACKLIGHT_LEVEL;
    d.touch = &touch;
    d.encoder = &encoder;
    d.button = &pcf.in(PCF_KNOB_SWITCH);
    d.knob.counts_per_detent = ENCODER_COUNTS_PER_DETENT;
    d.knob.invert = ENCODER_INVERT;
    d.refresh_ms = REFRESH_MS;
    d.after_first_frame = lcdPowerOffAfterInit;
    app::startDisplay(d, uiPort);

    Serial.println("BrewDial ready");
}

void loop()
{
    vTaskDelete(nullptr);   // everything runs in the brew tasks
}
