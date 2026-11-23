#include "brew/drivers/chips/mcp23008.h"

namespace brew::drivers {

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

}  // namespace brew::drivers
