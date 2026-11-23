#include "brew/drivers/chips/cst8xx.h"

namespace brew::drivers {

namespace {
constexpr uint8_t kCstRegCount = 0x02;
constexpr uint8_t kCstRegData = 0x03;
constexpr uint8_t kCstRegChipId = 0xAA;
constexpr uint8_t kCstMaxTouches = 5;
}  // namespace

bool Cst8xxTouch::begin()
{
    uint8_t id = 0;
    if(!bus_.writeRead(cfg_.addr, &kCstRegChipId, 1, &id, 1)) return false;
    chip_id_ = id;
    /* 0x11 CST826, 0xB4 CST816S, 0xB5 CST816T, 0xB6 CST816D, 0xB7 CST820 */
    return id == 0x11 || (id >= 0xB4 && id <= 0xB7);
}

bool Cst8xxTouch::read(hal::TouchPoint & out)
{
    uint8_t buf[1 + 6];
    {
        hal::I2cLock lock(bus_);
        if(!bus_.writeRead(cfg_.addr, &kCstRegCount, 1, buf, 1)) {
            if(!cfg_.nack_means_released) return false;
            out.pressed = false;
            return true;
        }
        uint8_t n = buf[0];
        if(n > kCstMaxTouches) n = 0;
        if(n == 0) {
            out.pressed = false;
            return true;
        }
        if(!bus_.writeRead(cfg_.addr, &kCstRegData, 1, buf + 1, 6)) return false;
    }
    int x = ((buf[1] & 0x0F) << 8) | buf[2];
    int y = ((buf[3] & 0x0F) << 8) | buf[4];
    if(cfg_.swap_xy) {
        const int t = x;
        x = y;
        y = t;
    }
    if(cfg_.mirror_x) x = cfg_.width - 1 - x;
    if(cfg_.mirror_y) y = cfg_.height - 1 - y;
    x += cfg_.x_offset;
    y += cfg_.y_offset;
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(x > cfg_.width - 1) x = cfg_.width - 1;
    if(y > cfg_.height - 1) y = cfg_.height - 1;
    out.x = static_cast<int16_t>(x);
    out.y = static_cast<int16_t>(y);
    out.pressed = true;
    return true;
}

}  // namespace brew::drivers
