/**
 * Elecrow CrowPanel 2.1" ESP32-S3 Rotary Display (480x480). Every pin and part number for
 * this board lives here or in main.cpp, nowhere else.
 *
 * Hardware (from Elecrow's factory sketch):
 *   - ST7701 RGB panel: 3-wire SPI init bus (CS 16, SCK 2, SDA 1) + 16-bit RGB
 *   - CST826 touch on I2C 0x15 (GPIO 38/39)
 *   - PCF8574 expander 0x21: P0 touch RST, P2 touch INT, P3 LCD power, P4 LCD RST, P5 knob switch
 *   - Rotary encoder A/B on GPIO 42/4, backlight PWM on GPIO 6
 *   - Relays/LEDs: MCP23008 at 0x20 on the same I2C bus (A2..A0 to GND; 0x21 is the PCF8574)
 */
#pragma once

/* ---------------- settings (overridable with -D in platformio.ini) ---------------- */
#ifndef BREW_DEMO
#define BREW_DEMO 1              /* 1: simulated fermenter (FakePlant) instead of a probe */
#endif
#ifndef ENCODER_INVERT
#define ENCODER_INVERT 0         /* 1 if turning clockwise lowers the target */
#endif
#define ENCODER_COUNTS_PER_DETENT 2   /* EC28A: 30 detents, 15 pulses/rev */
#define BACKLIGHT_LEVEL      204      /* 0..255 */
#define HISTORY_MINUTES      1        /* one graph sample (mean reading) every N minutes; window = 120 samples */
#define REFRESH_MS           16       /* LVGL refresh period */
#define TOUCH_Y_OFFSET       (-20)    /* Elecrow's touch offset */

/* ---------------- pins ---------------- */
#define I2C_SDA_PIN          38
#define I2C_SCL_PIN          39
#define ENCODER_A_PIN        42
#define ENCODER_B_PIN        4
#define BACKLIGHT_PIN        6
#define PROBE_PIN            (-1)     /* TODO: DS18B20 data pin once wired (needs BREW_DEMO 0) */

/* split topology (env crowpanel21_display): link to a controller board over UART.
 * TODO: confirm which header pins are free on your unit. */
#define LINK_UART_PORT       1
#define LINK_UART_TX         43
#define LINK_UART_RX         44
#define LINK_UART_BAUD       115200

/* ---------------- I2C parts ---------------- */
#define PCF8574_ADDR         0x21
#define PCF_TOUCH_RST        0
#define PCF_TOUCH_INT        2
#define PCF_LCD_POWER        3
#define PCF_LCD_RST          4
#define PCF_KNOB_SWITCH      5
#define TOUCH_ADDR           0x15
#define MCP23008_ADDR        0x20
#define MCP_PIN_HEAT         4        /* GP4: red LED (later: heat pad relay) */
#define MCP_PIN_COOL         5        /* GP5: green LED (later: glycol pump relay) */
