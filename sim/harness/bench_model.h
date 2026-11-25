/**
 * Display throttling profiles: how long a device needs to draw a frame, from the area the frame
 * renders. The PC's own speed says nothing about a board, but frame time there follows the
 * rendered area closely. Used by the web harness's Performance card (live throttling and the
 * benchmark), Harness::runBench and bench_check (which always uses the measured CrowPanel
 * profile).
 *
 * The CrowPanel profile was fitted 2026-11-25 to tools/device_bench.py (avg_ms vs
 * rendered_px_per_frame per phase, relative error minimised): within ~20% per phase (idle +7%,
 * turns -15..-20%, menu +22%, full redraw -6%). Refit with `tools/device_bench.py COMn --fit`
 * after display, LVGL or memory changes; add a measured profile per board the same way.
 * The lower-clock profiles are estimates: they assume every cost scales with the CPU clock,
 * which PSRAM and the panel's own timing don't fully do.
 */
#pragma once

namespace brew::sim {

struct ThrottleProfile {
    const char * id;
    const char * name;
    const char * note;
    double base_us;      // per frame
    double us_per_px;    // per rendered pixel
    bool measured;       // fitted to device runs (else an estimate)
};

inline constexpr ThrottleProfile kThrottleProfiles[] = {
    { "none", "No throttling", "Your PC's speed: frames only wait for LVGL's 16 ms refresh.", 0.0, 0.0, false },
    { "s3-240-rgb-psram", "ESP32-S3 240 MHz \xC2\xB7 RGB panel, PSRAM framebuffer",
      "CrowPanel 2.1\" (480\xC3\x97" "480). Measured: drawing time fitted to device runs, within ~20% per phase. "
      "Not yet modelled: the time spent on each input, so fast spinning looks ~3\xC3\x97 smoother here than on the board.",
      7530.0, 0.872, true },
    { "s3-160-rgb-psram", "ESP32-S3 160 MHz \xC2\xB7 RGB panel, PSRAM framebuffer",
      "Estimate: the CrowPanel's costs \xC3\x97 1.5, as if all of it scaled with the CPU clock.", 7530.0 * 1.5, 0.872 * 1.5,
      false },
    { "s3-80-rgb-psram", "ESP32-S3 80 MHz \xC2\xB7 RGB panel, PSRAM framebuffer",
      "Estimate: the CrowPanel's costs \xC3\x97 3, as if all of it scaled with the CPU clock.", 7530.0 * 3.0, 0.872 * 3.0,
      false },
};
inline constexpr int kThrottleProfileCount = sizeof(kThrottleProfiles) / sizeof(kThrottleProfiles[0]);
inline constexpr int kThrottleNone = 0;
inline constexpr int kThrottleCrowPanel = 1;   // bench_check's reference

}  // namespace brew::sim
