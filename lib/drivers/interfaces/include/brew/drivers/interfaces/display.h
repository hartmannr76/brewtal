/**
 * Display panel + backlight.
 *
 * A DisplayPanel takes RGB565 pixels in LVGL's native order and gets them onto the glass.
 * Anything panel-specific (byte swapping, R/B order, init sequences, reset/power pins)
 * lives inside the driver, never in app code.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace brew::hal {

struct Area {
    int16_t x1, y1, x2, y2;   // inclusive, like lv_area_t
};

class DisplayPanel {
public:
    /** Called when the pixel buffer passed to flush() may be reused. May run in an ISR. */
    using FlushDone = void (*)(void * ctx);

    virtual ~DisplayPanel() = default;

    virtual bool     begin() = 0;
    virtual uint16_t width() const = 0;
    virtual uint16_t height() const = 0;

    /**
     * Copy `px` (RGB565, row-major, area-sized) to the panel. The driver may modify `px`
     * in place (e.g. byte swap). Call done(ctx) when the buffer can be reused: right away
     * for framebuffer panels, from the DMA-complete callback for SPI/QSPI panels.
     */
    virtual void flush(const Area & area, uint16_t * px, FlushDone done, void * ctx) = 0;

    /** Hints for the LVGL port: render buffer height and where to allocate it. */
    virtual uint16_t drawBufferLines() const { return 40; }
    virtual uint32_t drawBufferCaps() const = 0;   // heap_caps flags on ESP; ignored on host
};

class Backlight {
public:
    virtual ~Backlight() = default;
    virtual void set(uint8_t level) = 0;   // 0 = off, 255 = full
};

}  // namespace brew::hal
