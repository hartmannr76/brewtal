# BrewDial on the Elecrow CrowPanel 2.1" Rotary Display

This port targets the 480×480 board: ESP32-S3R8 with 8 MB PSRAM and 16 MB flash, an ST7701 RGB panel, CST826 touch, a PCF8574 I/O expander, and the knob on GPIO 42/4 with its push switch on the expander. Pins, panel init sequence, timings, color order and touch offset come from Elecrow's factory sketch. The drivers themselves are now written against ESP-IDF (`esp_lcd`, I2C master, PCNT, LEDC) instead of Arduino_GFX and the Arduino libraries.

Everything board-specific is in two files: [`board_config.h`](board_config.h) (pins, addresses, settings) and [`main.cpp`](main.cpp) (which drivers, how they're wired together, power-up sequence).

Flashing replaces Elecrow's demo. To go back, flash `factory_firmware/firmware/RotaryScreen_2_1_*_merged.bin` from [their repo](https://github.com/Elecrow-RD/CrowPanel-2.1inch-HMI-ESP32-Rotary-Display-480-480-IPS-Round-Touch-Knob-Screen) the same way, at address `0x0`.

## Option A: flash a prebuilt image from the browser (no installs)

1. Plug the board into your PC with a USB-C data cable, using the port marked for USB/5V-IN.
2. In **Chrome or Edge**, open <https://espressif.github.io/esptool-js/>.
3. Set the baud rate to 921600, click **Connect**, and pick the board's COM port.
   - If no port appears, or it won't connect, put the board in download mode: hold **BOOT**, tap **RESET**, then release BOOT. You can also hold BOOT while plugging in the cable.
4. Under *Program*, set the flash address to **`0x0`** and choose `BrewDial-crowpanel21.bin`.
5. Click **Program**. When it finishes, press **RESET** on the board.

Get `BrewDial-crowpanel21.bin` from the project's GitHub releases, or build it yourself (Option B): it's `.pio/build/crowpanel21/firmware.factory.bin`. It's env `crowpanel21` in demo mode, so every state can be seen without a probe. Images are deliberately not committed to the repo, so a binary can't drift from the code that's meant to produce it. Locally, `prebuilt/` is ignored by git and is a handy place to keep one.

You should see a boot temperature of 68.4°. If the screen sits at **66°** and always says "Holding at …", the UI isn't getting any readings from the controller, which usually means you flashed an old image.

## Option B: build and upload with PlatformIO

`platformio.ini` at the repo root sets the board, libraries and `-O2`.

1. Install the **PlatformIO IDE** extension in VS Code, then open the repo root.
2. **Windows only:** turn on long paths once, from an admin PowerShell. The ESP32 framework has paths over 260 characters, and without this the install fails with `FileNotFoundError`:
   ```powershell
   Set-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem LongPathsEnabled 1
   ```
3. Plug the board in and run **Upload and Monitor** from the PlatformIO toolbar (env `crowpanel21`), or from a terminal:
   ```sh
   pio run -e crowpanel21 -t upload -t monitor
   ```
   The first build downloads about 1 GB of toolchain into `~/.platformio-pioarduino`. That's a separate core dir, so your global PlatformIO install and other projects aren't touched. On Windows, run it from PowerShell or the VS Code terminal. The ESP32 tool installer refuses to run under Git Bash or MSYS2.
4. In the monitor you should see an I2C scan, `display ready`, `BrewDial ready`, and a log line for every setpoint, mode or unit change.

It uses ESP32 Arduino core 3.3.11 (pioarduino `55.03.311`, ESP-IDF 5.5) only as the host, with LVGL 9.3.0 and `config/lv_conf.h` (the same as the simulator).

## Settings in `board_config.h`

| Define                      | Default | Meaning                                                        |
|-----------------------------|---------|----------------------------------------------------------------|
| `BREW_DEMO`                 | `1`     | Simulated fermenter (`FakePlant`). Set to `0` and set `PROBE_PIN` once a DS18B20 is wired |
| `PROBE_PIN`                 | `-1`    | DS18B20 data pin (needs a 4.7 k pull-up to 3V3)                |
| `ENCODER_INVERT`            | `0`     | Set to `1` if turning clockwise lowers the target              |
| `ENCODER_COUNTS_PER_DETENT` | `2`     | Quadrature counts per click. The EC28A has 30 clicks and 15 pulses per turn, which gives 2 |
| `REFRESH_MS`                | `16`    | LVGL refresh period. Elecrow's `lv_conf.h` defaults to 33       |
| `BACKLIGHT_LEVEL`           | `204`   | Backlight brightness, 0–255                                    |
| `HISTORY_MINUTES`           | `1`     | Minutes per graph sample (the mean reading). The graph shows 120 samples, so 1 = the last 2 h |
| `TOUCH_Y_OFFSET`            | `-20`   | Elecrow's touch calibration                                    |
| `MCP_PIN_HEAT` / `MCP_PIN_COOL` | `4` / `5` | Outputs on the MCP23008 at 0x20 (LEDs now, relays later) |

The render buffer height (48 lines, internal RAM) is `draw_lines` in `panelConfig()` in `main.cpp`. Animation lengths can be set with build flags: `-DBREW_FADE_MS=160` for view, ring and graph fades, and `-DBREW_BG_FADE_MS=350` for background color changes. Shorter fades re-render less.

## Controller

The controller is the portable `brew::Controller` (lib/brew_core). It is configured in `main.cpp`:

- **Sensor.** `FakePlant` in demo mode, `EspDs18b20` otherwise. Any `hal::TempSensor` works. If the reading goes invalid (three failed reads), every output turns off.
- **Control law.** `HysteresisLaw` with `ChamberDefaults`: heat on at target − 0.5 °F and cool on at target + 0.5 °F, each running until the target is reached. The original behaviour was `DeadbandLaw(0.3 °F)`, which short-cycles the relays.
- **Outputs.** `OutputGuard` drives the two MCP23008 pins, never both at once, with `ChamberDefaults::guard()`: cooling rests 5 min after it stops and after power-up (compressor delay), and heating and cooling are 10 min apart. Per-relay `min_on_ms` / `min_off_ms` are in `OutputGuard::Config`.
- **Settings.** Setpoints, mode, pause and units are saved in NVS and restored at boot.

## Running display and controller on separate boards

Build env `crowpanel21_display` for the CrowPanel and `controller_devkit` for any ESP32-S3 dev board with the probe and relays. Connect them over UART: TX to RX, RX to TX, common GND. The CrowPanel side uses `LINK_UART_*` from `board_config.h`. **Check that GPIO 43/44 are free on your unit before relying on them.** The display asks for a full sync at boot and whenever the controller goes quiet for 3 s.

## Knob and performance notes

- **Knob.** Elecrow's sample polls GPIO 42 every 2 ms and counts only rising edges of channel A. On this encoder that catches roughly every other click, which is the "turn it twice to register one" behavior. Here the ESP32-S3 pulse counter (PCNT) decodes both channels in hardware, with a glitch filter and the internal pull-ups on. `KnobInput` turns counts into one step per click, and bounce cancels itself out. A click followed immediately by a turn is never reordered (unit-tested in `test/test_input.cpp`).
- **Colors.** The panel wants R and B swapped. Elecrow's driver did that per pixel on every flush; here the RGB data pins are simply assigned in swapped order (`swap_rb`), so it costs nothing.
- **Rendering.** LVGL draws into small buffers in internal SRAM, which is much faster than PSRAM. Each refresh redraws only what changed. The four state backgrounds are pre-rendered at boot, and the graph re-renders at most every 10 s. Turning the knob while adjusting redraws about 15% of the screen per click. The full-screen work left is the short fade when you start adjusting or open the menu.

## Changing the UI

The UI lives in `lib/brew_ui/` and is shared by every board and the desktop simulator, so there's nothing to copy or sync. Build the simulator at 480 to match this board (the default): `cmake -S sim -B sim/build -DBREW_DISP_SIZE=480`.

## What's verified and what isn't

- **Builds:** all four envs (`crowpanel21`, `crowpanel21_display`, `controller_devkit`, `waveshare_knob18`) compile with pioarduino 55.03.311.
- **Host:** the unit tests pass, and the scripted simulator session renders pixel-identical frames to the pre-refactor code, through the new controller → bus → UI bridge path.
- **Hardware (2026-11-13):** env `crowpanel21` in demo mode runs on a physical CrowPanel. Display, touch, knob, backlight, and the controller driving the simulated fermenter all work.
- **Not yet tested on hardware:** the DS18B20 probe (`BREW_DEMO 0`), real relays on the MCP23008, and the split display/controller setup over UART (`crowpanel21_display` + `controller_devkit`).
