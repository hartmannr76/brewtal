# Web test harness

The display firmware and the controller firmware, compiled to WebAssembly and running in a browser page on simulated hardware. Use it to try UI and control changes without flashing a board.

Everything except the drivers is the real code: `brew::Controller`, the control laws, `OutputGuard`, `KnobInput`, `TouchTracker`, `UiBridge`, `brew_ui` with LVGL, and the board-to-board link (`StreamPacketLink` and the remote proxies). The drivers are replaced by the fakes in [`../harness/sim_devices.h`](../harness/sim_devices.h). So what you see is pixel for pixel what the panel shows. What the harness can't tell you: real driver timing (I²C, panel setup, 1-Wire), and how fast rendering is on the chip.

## Run it

```powershell
powershell -File tools/web.ps1          # builds sim/build-web-480, then opens the page
```

The page is one self-contained file set, so `index.html` opens straight from disk with no web server.

**One-time setup:**
1. Install the Emscripten SDK into `%USERPROFILE%\emsdk`:
   ```powershell
   git clone https://github.com/emscripten-core/emsdk.git $env:USERPROFILE\emsdk
   & $env:USERPROFILE\emsdk\emsdk.bat install latest
   & $env:USERPROFILE\emsdk\emsdk.bat activate latest
   ```
2. You also need `ninja` or GNU `make` on `PATH`.

**Manual build:** activate the SDK with `emsdk_env.ps1` first, then:
```sh
emcmake cmake -S sim -B sim/build-web -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM=make
cmake --build sim/build-web
```

**Headless check** (runs as part of verifying changes):
```sh
node sim/harness/web_smoke.js sim/build-web-480/web/brew_web.js
```
The native build also runs `harness_check` through ctest, which covers the same paths in more depth.

## What you can do

| Panel | Controls |
|---|---|
| Screen | Drag the knurled ring around the glass to turn the knob (12° per detent, like the CrowPanel's 30-detent ring), or scroll. Click or drag the glass to touch. ←/→ turn, Space presses (hold it to hold the knob down). |
| Device | **Round · knob ring** or **Square · buttons** (remembered; `index.html?device=square` picks it). Square shows a square bezel with three side keys (up, down, and select, whose name is still to be decided) and the deck controls change to match. They're real press-and-hold keys that go through the firmware's `ButtonInput` (debounce, hold-to-repeat). ↑/↓ and Space/Enter work too. The screen switches to the firmware's square layout (`layout_square.c`) live, through `brew_ui_set_layout()`, so whatever you were doing carries over. |
| Knob buttons | ±1 and ±5 detents. **½ detent** sends one raw encoder count, which must do nothing. **Press** can be held. |
| Speed | Pause, or ×1 to ×3600. Only the controller and the beer speed up; the UI and knob stay in real time. `index.html?speed=600` starts fast. |
| Beer & probe | Drag the beer temperature. Turn physics off to pin the temperature where you put it. Probe faults: disconnected, stuck, noisy (with σ). Calibration offset. |
| Fermenter | The physical setup the beer follows (`FakePlant`, a heat balance): batch size, room or chamber temperature, insulation (bare, neoprene jacket, foam jacket, chest freezer, or heat loss in W/°C), heater and cooling power, each with its own lag (a heat wrap keeps heating after it switches off), and yeast heat. Presets for common rigs. Below the settings it shows what they mean: °F/h heating and cooling, how fast the beer drifts to room temperature, and the warmest/coldest it can hold. |
| Outputs | Heat/cool relay lights, time in the current state, switch count and switches per hour, total on time, and how long a rest or heat↔cool gap is still holding a relay off. |
| Controller setup | **Recommended** (what the boards run, `ChamberDefaults`) and **Original** (the old ±0.3 °F deadband) presets. Or set them yourself: control law (differential: run to target; deadband: stop at the band), heat and cool differentials, min on/off per relay (cool min off = compressor delay, also counted from power-up), heat↔cool gap, history interval. **Apply & reboot** restarts the simulated controller. Settings survive a reboot like NVS does; history doesn't. **Factory reset** wipes settings. |
| Display ↔ controller | Same board (`LocalBus`), or two boards over a simulated cable carrying the real COBS+CRC framing. Faults: unplug, byte loss, bit errors. Counters show bytes and rejected frames. |
| History / Events | A strip chart of beer, probe reading, target, and heating/cooling. An event log of every command the UI sends and every controller decision. |

## Look

The page takes its colour from the dial: the room light, primary buttons, switches and chart use the same gradients `brew_ui.c` uses for cool/idle, heat, cold crash and pause (`s_grad`, `s_line`), and change when the dial does. Fonts come from Google Fonts (Inter Tight for numbers, Instrument Sans for text, JetBrains Mono for the log); offline the page falls back to system fonts. The palette tokens are at the top of `style.css`.

## Where things live

- `sim/harness/harness.{h,cpp}`: wires both firmware sides to the fake devices and manages the two clocks.
- `sim/harness/web_api.cpp`: the flat `bd_*` C API the page calls. The native smoke test calls it too.
- `sim/web/`: the page (`index.html`, `app.js`, `style.css`), copied next to `brew_web.js` at build time.

To add a new simulated part:
1. Implement its `brew_hal` interface in `sim_devices.h`.
2. Wire it in `Harness`.
3. Expose a `bd_*` setter.
4. Add a control in `index.html` and `app.js`.
