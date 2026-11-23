#include "brew/drivers/chips/pcf8574.h"

namespace brew::drivers {

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

}  // namespace brew::drivers
