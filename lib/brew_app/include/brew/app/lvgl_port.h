/**
 * hal::DisplayPanel -> lv_display, queued touch InputEvents -> LVGL pointer indev.
 * Portable: the caller allocates the render buffers (ESP: internal SRAM / DMA caps; host: anything).
 */
#pragma once

#include "brew/hal/display.h"
#include "brew/input.h"
#include "brew/local_bus.h"

#include "lvgl.h"

namespace brew::app {

class LvglPort {
public:
    /** Call after lv_init(). buf2 may be null. `bytes` is the size of each buffer. */
    lv_display_t * begin(hal::DisplayPanel & panel, void * buf1, void * buf2, size_t bytes, uint32_t refresh_ms);

    /** Creates a pointer indev fed by pushTouch(). */
    lv_indev_t * addTouch();

    /** Queue a TouchDown/Move/Up for the indev (call from the LVGL task). */
    void pushTouch(const InputEvent & e) { touch_.push(e); }

    lv_display_t * display() const { return disp_; }

private:
    static void flushCb(lv_display_t * d, const lv_area_t * a, uint8_t * px);
    static void flushDone(void * ctx);
    static void touchCb(lv_indev_t * indev, lv_indev_data_t * data);

    hal::DisplayPanel * panel_ = nullptr;
    lv_display_t * disp_ = nullptr;
    SpscRing<InputEvent, 32> touch_;
    InputEvent last_{};
    bool pressed_ = false;
};

}  // namespace brew::app
