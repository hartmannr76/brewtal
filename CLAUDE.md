# BrewDial: working rules

Round fermentation-controller UI (LVGL 9) + controller firmware for ESP32-S3 boards.
Build with **PlatformIO only** (`pio run -e <env>`); no Arduino IDE route. Host sim/tests via CMake in `sim/`.

`tools/check_rules.py` enforces most of the rules below and runs before every PlatformIO build and every
CMake build. Never weaken or bypass it to make a build pass. Fix the code, or change the rule here *and*
in the script on purpose.

## Layout and layering

```
lib/drivers/interfaces  driver interfaces only (DisplayPanel, Touch, Encoder, DigitalIn/Out, I2cBus,
                        ByteStream, TempSensor, SettingsStore, Link, Wifi, HttpServer, WallClock),
                        namespace brew::hal, included as
                        brew/drivers/interfaces/*.h. No implementations, no platform includes.
lib/core                portable logic: Controller, ControlLaw, OutputGuard, History, KnobInput/TouchTracker,
                        codec, framing, LocalBus, remote proxies, FakePlant. Includes only std + interfaces.
lib/drivers/chips       portable chip drivers on interface buses, one file per chip (pcf8574, mcp23008,
                        cst8xx). Host-testable.
lib/drivers/esp32       ESP-IDF drivers, one file per peripheral or part (i2c, gpio, pcnt_encoder,
                        ledc_backlight, rgb_panel, qspi_panel, st7701, uart, nvs_settings, ds18b20, wifi,
                        http_server, udp_responder, sntp_clock) + task.h
                        (startPeriodic, delayMs). ESP-IDF APIs only: never Arduino APIs.
lib/ui                  the C view. brew_model.c = UI state + rules (no LVGL, unit-tested); layouts
                        (layout_round.c, layout_square.c, sharing layout_kit.c) draw it behind
                        brew_layout.h; brew_ui.c = API glue.
                        Knows nothing about controller/drivers.
lib/network             WiFi setup: WifiSetup (setup network -> phone -> connect -> reconnect), SetupPortal
                        (the setup page's HTTP handler, no HTTP library) and the page itself
                        (web/setup.html, embedded by tools/gen_setup_page.py into src/setup_page.cpp;
                        check_rules.py fails the build if that copy is stale). Includes only std +
                        interfaces. Runs on the display board.
lib/fonts/<size>        generated fonts (tools/gen_fonts.sh); an env lists exactly one.
lib/app                 UiBridge (brew_ui <-> UiPort), LvglPort, ESP display runtime. Never includes drivers.
                        Folders carry no brew_ prefix; each library.json names it brewdial-<...>, and
                        platformio.ini envs list libraries by that name.
boards/<name>/          composition roots: the ONLY place pins, part numbers, addresses and driver choices
                        live. One PlatformIO env per board/role.
proto/brewdial.proto    schema of record for board-to-board messages (not compiled yet).
sim/  test/             host simulator (SDL desk, headless sim, bench) and unit tests (brew_tests).
sim/harness sim/web     web test harness: real firmware on fake devices (sim_devices.h), bd_* C API,
                        browser page. New driver interfaces get a fake there so the harness can drive them.
```

- New hardware means a new driver implementing a `lib/drivers/interfaces` interface (in `lib/drivers/chips` or
  `lib/drivers/<platform>`, one file per part) plus wiring in `boards/<name>/main.cpp`.
  App/core code never `#if`s on a board.
- UI and controller talk **only** through `UiPort` / `ControlPort` (`State`, `Command`, `HistoryChunk`).
  Same board: `LocalBus`. Separate boards: `RemoteControllerProxy` / `RemoteUiProxy` over a `hal::Link`.
- Temperatures are °C (float) everywhere in core; `UiBridge` converts to brew_ui's °F API.
- LVGL / brew_ui calls happen only in the UI task. The controller runs in its own task.
- Policy (debounce, press/turn ordering, interlocks, min on/off times) lives in core and is unit-tested,
  not in drivers.
- UI behaviour (what a turn/press does, auto-commit, menu and settings lists, editors) lives in `brew_model.c`, never in a
  layout. A new screen shape is a new `brew_layout_t`; it must not change behaviour.

## LVGL redraw cost

Frame time is set by how much of the screen LVGL repaints, and a full repaint costs ~200 ms (~5 fps) on the
CrowPanel. LVGL repaints the *whole box* of any object it thinks changed a lot, even for an instant mid-update:

- Arcs: `lv_arc_set_angles()` moves the end first. Moving an arc backwards, the end passes the start for a
  moment, the arc reads as a full circle and its whole box (the whole dial) is repainted. That made turning the
  knob down 3-4x slower than up. Use `kit_arc_set_angles()`, which moves the leading edge first;
  `check_rules.py` rejects `lv_arc_set_angles` elsewhere.
- Setting a style, text or colour to the value it already has still repaints: use the `kit_set_*` helpers,
  which skip unchanged values.
- Fades and opacity animations repaint everything under the object every frame; keep them short and hide
  objects that are fully transparent.

Measure, don't guess: `tools/device_bench.py` (below) reports px repainted per frame for each phase.

## Wire format rules (keep the nanopb migration a one-file change)

1. App code never touches the wire format. Only `lib/core/src/codec.cpp` turns domain types into
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

## Generated files

- `lib/fonts/<size>/src/*.c`: `tools/gen_fonts.sh <size>`.
- `lib/network/src/setup_page.cpp`: edit `lib/network/web/setup.html`, then `python tools/gen_setup_page.py`.
  The page is served on a network without internet: keep it self-contained (no external fonts or scripts).

## Verifying changes

- `python tools/check_rules.py`
- Host: `cmake -S sim -B sim/build && cmake --build sim/build --config Release`, then run `brew_tests`
  (and `brew_sim <outdir>` to render the scripted session). `ctest` also runs `harness_check`.
- Web harness: `powershell -File tools/web.ps1 -NoOpen`, then
  `node sim/harness/web_smoke.js sim/build-web-480/web/brew_web.js`.
- Firmware: `pio run -e crowpanel21` (also `crowpanel21_demo`, `crowpanel21_display`, `controller_devkit`,
  `controller_devkit_demo`, `waveshare_knob18`). `*_demo` = simulated fermenter instead of the probe; the dial
  says DEMO. A real build with no `PROBE_PIN` runs with no reading (outputs off, dial shows NO READING).
- On the device: `pio run -e crowpanel21 -t upload --upload-port COMn`, then `tools/device_log.py COMn` (run
  with the pioarduino penv Python) resets the board and saves its serial log. Startup logs each stage with
  ms since boot and internal RAM left, so that's where to look for boot time and RAM headroom.
- UI performance: `tools/device_bench.py COMn` flashes `crowpanel21_bench`, runs the scripted benchmark (idle,
  knob up/down, menu, full redraw; `lib/app/src/bench.cpp`), compares with the last run and appends to
  `bench/results.csv` (commit it with the change). `--check` fails on a >15% regression in fps, frame time,
  boot time or free RAM. Run it after UI, LVGL, display-driver or memory changes.
- Without a device: ctest's `bench_check` runs the same benchmark on the host harness with a CrowPanel-paced
  virtual clock and compares the work per frame (rendered/invalidated px, exact and repeatable) with
  `bench/host_baseline.csv`; it fails on >5% more work. If the increase is intended, run
  `sim/build-msvc/Release/bench_check bench/host_baseline.csv --update-baseline` and commit the baseline with the
  change: never update it just to make the check pass. Its fps/ms are estimates (`sim/harness/bench_model.h`,
  refit with `device_bench.py --fit`). The web harness's Benchmark card runs it too.
