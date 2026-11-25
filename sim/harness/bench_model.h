/**
 * Estimated CrowPanel (480x480) frame time from the area a frame renders, for host benchmark
 * runs (Harness::runBench, bench_check, the web harness). The PC's own speed says nothing about
 * the device, but frame time there follows the rendered area closely.
 *
 * Fitted 2026-11-25 to tools/device_bench.py (avg_ms vs rendered_px_per_frame per phase, relative
 * error minimised): within ~20% per phase (idle +7%, turns -15..-20%, menu +22%, full redraw -6%).
 * Refit with `tools/device_bench.py COMn --fit` after display, LVGL or memory changes.
 */
#pragma once

namespace brew::sim {

constexpr double kBenchBaseUs = 7530.0;   // per frame
constexpr double kBenchUsPerPx = 0.872;   // per rendered pixel

}  // namespace brew::sim
