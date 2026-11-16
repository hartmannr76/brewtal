# BrewDial: working rules

Round fermentation-controller UI (LVGL 9) + controller firmware for ESP32-S3 boards.
Build with **PlatformIO only** (`pio run -e <env>`); no Arduino IDE route. Host sim/tests via CMake in `sim/`.

`tools/check_rules.py` enforces most of the rules below and runs before every PlatformIO build and every
CMake build. Never weaken or bypass it to make a build pass. Fix the code, or change the rule here *and*
in the script on purpose.

## Layout and layering

```
lib/brew_hal            interfaces only (DisplayPanel, Touch, Encoder, DigitalIn/Out, I2cBus, ByteStream,
                        TempSensor, SettingsStore, Link). No implementations, no platform includes.
lib/brew_core           portable logic: Controller, ControlLaw, OutputGuard, History, KnobInput/TouchTracker,
                        codec, framing, LocalBus, remote proxies, FakePlant. Includes only std + brew_hal.
lib/brew_drivers        portable chip drivers on hal buses (PCF8574, MCP23008, CST8xx). Host-testable.
lib/brew_drivers_esp32  ESP-IDF peripheral drivers (I2C master, GPIO, PCNT, LEDC, esp_lcd RGB/QSPI, UART,
                        NVS, DS18B20) + startPeriodic(). ESP-IDF APIs only: never Arduino APIs.
lib/brew_ui             the C view. brew_model.c = UI state + rules (no LVGL, unit-tested); layouts
                        (layout_round.c, layout_square.c, sharing layout_kit.c) draw it behind
                        brew_layout.h; brew_ui.c = API glue.
                        Knows nothing about controller/hal/drivers.
lib/brew_fonts_<size>   generated fonts; an env lists exactly one.
lib/brew_app            UiBridge (brew_ui <-> UiPort), LvglPort, ESP display runtime. Never includes drivers.
boards/<name>/          composition roots: the ONLY place pins, part numbers, addresses and driver choices
                        live. One PlatformIO env per board/role.
proto/brewdial.proto    schema of record for board-to-board messages (not compiled yet).
sim/  test/             host simulator (SDL desk, headless sim, bench) and unit tests (brew_tests).
sim/harness sim/web     web test harness: real firmware on fake hal devices (sim_devices.h), bd_* C API,
                        browser page. New hal interfaces get a fake there so the harness can drive them.
```

- New hardware means a new driver implementing a `brew_hal` interface plus wiring in `boards/<name>/main.cpp`.
  App/core code never `#if`s on a board.
- UI and controller talk **only** through `UiPort` / `ControlPort` (`State`, `Command`, `HistoryChunk`).
  Same board: `LocalBus`. Separate boards: `RemoteControllerProxy` / `RemoteUiProxy` over a `hal::Link`.
- Temperatures are °C (float) everywhere in core; `UiBridge` converts to brew_ui's °F API.
- LVGL / brew_ui calls happen only in the UI task. The controller runs in its own task.
- Policy (debounce, press/turn ordering, interlocks, min on/off times) lives in core and is unit-tested,
  not in drivers.
- UI behaviour (what a turn/press does, auto-commit, menu, scrub) lives in `brew_model.c`, never in a
  layout. A new screen shape is a new `brew_layout_t`; it must not change behaviour.

## Wire format rules (keep the nanopb migration a one-file change)

1. App code never touches the wire format. Only `lib/brew_core/src/codec.cpp` turns domain types into
   bytes and back. `wire_v*.h` is private to it.
2. No `memcpy`/casts of structs to bytes, no packed structs, no bitfields, no `sizeof(State)` etc. outside
   the codec. Write fields one by one, little-endian, in fixed-width integers (temperatures as
   centi-°C `i16`, not floats).
3. Every frame starts with `[proto_ver:u8][msg_type:u8][seq:u16]`. nanopb = new `proto_ver` + a new codec.
4. Field numbers in `wire_v1.h` (`#n name`) match `proto/brewdial.proto` exactly. Change both together;
   the checker compares them.
5. Within a version, only append fields at the end of a payload. Never reorder, resize, renumber or reuse.
   Decoders ignore extra trailing bytes and default missing trailing fields.
6. The golden-byte tests in `test/test_wire.cpp` pin v1. If one fails, you broke compatibility with
   deployed boards: bump `proto_ver` instead of editing the expected bytes.
7. Commands are idempotent ("set X to Y", never "+0.5"), and the controller re-publishes `State`
   periodically, so lost or duplicated messages are harmless.
8. Settings are persisted as typed key/value pairs (`SettingsStore::setI32/setStr`), never as struct blobs.

## Verifying changes

- `python tools/check_rules.py`
- Host: `cmake -S sim -B sim/build && cmake --build sim/build --config Release`, then run `brew_tests`
  (and `brew_sim <outdir>` to render the scripted session). `ctest` also runs `harness_check`.
- Web harness: `powershell -File tools/web.ps1 -NoOpen`, then
  `node sim/harness/web_smoke.js sim/build-web-480/web/brew_web.js`.
- Firmware: `pio run -e crowpanel21` (also `crowpanel21_display`, `controller_devkit`, `waveshare_knob18`).
