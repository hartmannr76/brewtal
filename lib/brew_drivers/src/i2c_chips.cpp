#include "brew/drivers/i2c_chips.h"

namespace brew::drivers {

/* -------------------------------- PCF8574 -------------------------------- */

Pcf8574::Pcf8574(hal::I2cBus & bus, uint8_t addr, uint8_t input_mask)
    : bus_(bus), addr_(addr), inputs_(input_mask), latch_(input_mask)
{
    for(uint8_t i = 0; i < 8; i++) {
        pins_[i].chip = this;
        pins_[i].n = i;
    }
}

bool Pcf8574::begin()
{
    latch_ = inputs_;
    return bus_.write(addr_, &latch_, 1);
}

bool Pcf8574::write(uint8_t pin, bool high)
{
    hal::I2cLock lock(bus_);
    const uint8_t bit = static_cast<uint8_t>(1u << (pin & 7));
    latch_ = static_cast<uint8_t>(high ? (latch_ | bit) : (latch_ & ~bit));
    latch_ |= inputs_;   // inputs must stay released
    return bus_.write(addr_, &latch_, 1);
}

bool Pcf8574::read(uint8_t pin, bool & high)
{
    uint8_t v = 0;
    if(!bus_.read(addr_, &v, 1)) return false;
    high = (v >> (pin & 7)) & 1u;
    return true;
}

/* -------------------------------- MCP23008 ------------------------------- */

namespace {
constexpr uint8_t kMcpIodir = 0x00;
constexpr uint8_t kMcpOlat = 0x0A;
}  // namespace

Mcp23008::Mcp23008(hal::I2cBus & bus, uint8_t addr, uint8_t output_mask)
    : bus_(bus), addr_(addr), outputs_(output_mask)
{
    for(uint8_t i = 0; i < 8; i++) {
        pins_[i].chip = this;
        pins_[i].n = i;
    }
}

bool Mcp23008::begin()
{
    hal::I2cLock lock(bus_);
    /* latch low first so the outputs come up off, then make the output pins outputs */
    const uint8_t olat[2] = { kMcpOlat, want_ };
    const uint8_t iodir[2] = { kMcpIodir, static_cast<uint8_t>(~outputs_) };
    configured_ = bus_.write(addr_, olat, 2) && bus_.write(addr_, iodir, 2);
    have_ = configured_ ? want_ : -1;
    return configured_;
}

void Mcp23008::write(uint8_t pin, bool high)
{
    const uint8_t bit = static_cast<uint8_t>(1u << (pin & 7));
    want_ = static_cast<uint8_t>(high ? (want_ | bit) : (want_ & ~bit));
    flush();
}

bool Mcp23008::flush()
{
    if(!configured_) return begin();
    if(have_ == want_) return true;
    const uint8_t olat[2] = { kMcpOlat, want_ };
    if(bus_.write(addr_, olat, 2)) {
        have_ = want_;
        return true;
    }
    configured_ = false;   // chip reset or unplugged: re-configure on the next write
    return false;
}

/* --------------------------------- CST8xx -------------------------------- */

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
