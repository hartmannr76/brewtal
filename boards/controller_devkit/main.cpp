/**
 * Headless controller board (any ESP32-S3 dev board): temperature probe + two relays, and a
 * UART link to a display board (e.g. env crowpanel21_display). No LVGL, no UI code.
 *
 *   display TX -> controller RX, display RX <- controller TX, common GND.
 */
#include <Arduino.h>   // only for setup()/loop() and Serial

#include "brew/controller.h"
#include "brew/drivers/esp32/ds18b20.h"
#include "brew/drivers/esp32/gpio.h"
#include "brew/drivers/esp32/nvs_settings.h"
#include "brew/drivers/esp32/uart.h"
#include "brew/drivers/esp32/task.h"
#include "brew/fake_plant.h"
#include "brew/link.h"

/* ---------------- settings + pins ---------------- */
#ifndef BREW_DEMO
#define BREW_DEMO 1          /* 1: simulated fermenter instead of a probe */
#endif
#define PROBE_PIN       5    /* DS18B20 data (4.7 k pull-up to 3V3) */
#define HEAT_RELAY_PIN  6
#define COOL_RELAY_PIN  7
#define LINK_UART_PORT  1
#define LINK_UART_TX    17
#define LINK_UART_RX    18
#define LINK_UART_BAUD  115200

using namespace brew;
using namespace brew::drivers;

namespace {

EspGpioOut heatRelay(HEAT_RELAY_PIN);
EspGpioOut coolRelay(COOL_RELAY_PIN);
NvsSettingsStore settings("brewdial");

EspUartStream uart({ .port = LINK_UART_PORT, .tx = LINK_UART_TX, .rx = LINK_UART_RX, .baud = LINK_UART_BAUD });
StreamPacketLink uartLink(uart);
RemoteUiProxy display(uartLink);

#if BREW_DEMO
FakePlant plant({});
hal::TempSensor & sensor = plant;
hal::DigitalOut & heatOut = plant.heat(&heatRelay);
hal::DigitalOut & coolOut = plant.cool(&coolRelay);
#else
EspDs18b20 probe({ .pin = PROBE_PIN });
hal::TempSensor & sensor = probe;
hal::DigitalOut & heatOut = heatRelay;
hal::DigitalOut & coolOut = coolRelay;
#endif

HysteresisLaw law(ChamberDefaults::kHeatDiffF * 5.0f / 9.0f, ChamberDefaults::kCoolDiffF * 5.0f / 9.0f);
/* A glycol pump doesn't need protecting; for a compressor use e.g. { 0, 5 * 60000, 0 }. */
OutputGuard guard(&heatOut, &coolOut, ChamberDefaults::guard());
Controller controller(display, sensor, law, guard, &settings, Controller::Config{});

}  // namespace

void setup()
{
    Serial.begin(115200);
    heatRelay.begin();
    coolRelay.begin();
    settings.begin();
    if(!uartLink.begin()) Serial.println("UART link init failed");
    startPeriodic("ctl", [](void * c, uint32_t now) { static_cast<Controller *>(c)->tick(now); }, &controller, 20);
    Serial.println("BrewDial controller ready");
}

void loop()
{
    vTaskDelete(nullptr);
}
