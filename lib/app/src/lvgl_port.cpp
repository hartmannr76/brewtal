#include "brew/app/lvgl_port.h"

namespace brew::app {

lv_display_t * LvglPort::begin(hal::DisplayPanel & panel, void * buf1, void * buf2, size_t bytes, uint32_t refresh_ms)
{
    panel_ = &panel;
    disp_ = lv_display_create(panel.width(), panel.height());
    lv_display_set_color_format(disp_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_user_data(disp_, this);
    lv_display_set_flush_cb(disp_, flushCb);
    lv_display_set_buffers(disp_, buf1, buf2, static_cast<uint32_t>(bytes), LV_DISPLAY_RENDER_MODE_PARTIAL);
    if(refresh_ms) lv_timer_set_period(lv_display_get_refr_timer(disp_), refresh_ms);
    return disp_;
}

void LvglPort::flushDone(void * ctx)
{
    lv_display_flush_ready(static_cast<lv_display_t *>(ctx));
}

void LvglPort::flushCb(lv_display_t * d, const lv_area_t * a, uint8_t * px)
{
    auto * self = static_cast<LvglPort *>(lv_display_get_user_data(d));
    const hal::Area area = { static_cast<int16_t>(a->x1), static_cast<int16_t>(a->y1),
                             static_cast<int16_t>(a->x2), static_cast<int16_t>(a->y2) };
    self->panel_->flush(area, reinterpret_cast<uint16_t *>(px), flushDone, d);
}

lv_indev_t * LvglPort::addTouch()
{
    lv_indev_t * t = lv_indev_create();
    lv_indev_set_type(t, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(t, this);
    lv_indev_set_read_cb(t, touchCb);
    lv_indev_set_display(t, disp_);
    return t;
}

/* Queued transitions are replayed in order. continue_reading makes LVGL call us again in the
 * same cycle, so a down+up that both happened during one frame still makes a tap. */
void LvglPort::touchCb(lv_indev_t * indev, lv_indev_data_t * data)
{
    auto * self = static_cast<LvglPort *>(lv_indev_get_user_data(indev));
    InputEvent e;
    if(self->touch_.pop(e)) {
        self->last_ = e;
        self->pressed_ = e.kind != InputEvent::Kind::TouchUp;
        data->continue_reading = !self->touch_.empty();
    }
    data->point.x = self->last_.a;
    data->point.y = self->last_.b;
    data->state = self->pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

}  // namespace brew::app
