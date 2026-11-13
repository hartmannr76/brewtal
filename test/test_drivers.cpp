/* Portable chip drivers against a fake I2C bus. */
#include "brew/drivers/i2c_chips.h"
#include "minitest.h"

#include <map>
#include <vector>

using namespace brew;
using namespace brew::drivers;

namespace {

/* Records writes; serves reads from per-address register maps (or a raw byte). */
struct FakeBus final : hal::I2cBus {
    std::map<uint8_t, std::vector<std::vector<uint8_t>>> writes;
    std::map<uint8_t, std::map<uint8_t, std::vector<uint8_t>>> regs;
    std::map<uint8_t, uint8_t> raw;
    std::map<uint8_t, bool> absent;
    bool write(uint8_t a, const uint8_t * d, size_t n) override
    {
        if(absent[a]) return false;
        writes[a].push_back(std::vector<uint8_t>(d, d + n));
        return true;
    }
    bool writeRead(uint8_t a, const uint8_t * w, size_t, uint8_t * r, size_t rn) override
    {
        if(absent[a]) return false;
        const auto & v = regs[a][w[0]];
        for(size_t i = 0; i < rn; i++) r[i] = i < v.size() ? v[i] : 0;
        return true;
    }
    bool read(uint8_t a, uint8_t * r, size_t rn) override
    {
        if(absent[a]) return false;
        for(size_t i = 0; i < rn; i++) r[i] = raw[a];
        return true;
    }
    bool probe(uint8_t a) override { return !absent[a]; }
    void lock() override {}
    void unlock() override {}
};

}  // namespace

TEST(pcf8574_keeps_inputs_high)
{
    FakeBus bus;
    Pcf8574 pcf(bus, 0x21, 1u << 5);
    CHECK(pcf.begin());
    CHECK_EQ(bus.writes[0x21].back()[0], static_cast<uint8_t>(0x20));
    pcf.out(3).write(true);
    CHECK_EQ(bus.writes[0x21].back()[0], static_cast<uint8_t>(0x28));
    pcf.out(3).write(false);
    CHECK_EQ(bus.writes[0x21].back()[0], static_cast<uint8_t>(0x20));
    bus.raw[0x21] = 0x00;          // P5 pulled low (knob pressed)
    CHECK(!pcf.in(5).read());
    bus.absent[0x21] = true;       // glitch: keep the last level
    CHECK(!pcf.in(5).read());
}

TEST(mcp23008_configures_and_recovers)
{
    FakeBus bus;
    bus.absent[0x20] = true;
    Mcp23008 mcp(bus, 0x20, (1u << 4) | (1u << 5));
    CHECK(!mcp.begin());
    mcp.out(4).write(true);        // still missing: remembered, not written
    CHECK(!mcp.configured());
    bus.absent[0x20] = false;
    mcp.out(5).write(false);       // retries: configure with OLAT = want (bit 4 set)
    CHECK(mcp.configured());
    const auto & w = bus.writes[0x20];
    CHECK_EQ(w.size(), static_cast<size_t>(2));
    CHECK(w[0] == (std::vector<uint8_t>{ 0x0A, 0x10 }));   // OLAT first, outputs off/as wanted
    CHECK(w[1] == (std::vector<uint8_t>{ 0x00, 0xCF }));   // IODIR: GP4, GP5 outputs
    mcp.out(5).write(true);
    CHECK(bus.writes[0x20].back() == (std::vector<uint8_t>{ 0x0A, 0x30 }));
    const size_t n = bus.writes[0x20].size();
    mcp.out(5).write(true);        // unchanged: no bus traffic
    CHECK_EQ(bus.writes[0x20].size(), n);
}

TEST(cst8xx_reads_and_transforms)
{
    FakeBus bus;
    bus.regs[0x15][0xAA] = { 0x11 };
    bus.regs[0x15][0x02] = { 1 };
    bus.regs[0x15][0x03] = { 0x81, 0x2C, 0x00, 0x0A, 0, 0 };   // x = 0x12C = 300, y = 10
    Cst8xxTouch::Config cfg;
    cfg.y_offset = -20;
    Cst8xxTouch t(bus, cfg);
    CHECK(t.begin());
    hal::TouchPoint p;
    CHECK(t.read(p));
    CHECK(p.pressed);
    CHECK_EQ(p.x, static_cast<int16_t>(300));
    CHECK_EQ(p.y, static_cast<int16_t>(0));   // 10 - 20 clamped
    bus.regs[0x15][0x02] = { 0 };
    CHECK(t.read(p));
    CHECK(!p.pressed);
}
