/* Codec (incl. golden v1 bytes), COBS/CRC framing, StreamPacketLink, remote proxies end to end. */
#include "brew/codec.h"
#include "brew/controller.h"
#include "brew/link.h"
#include "minitest.h"

#include <cstring>
#include <deque>
#include <vector>

using namespace brew;

namespace {

std::vector<uint8_t> bytes(const uint8_t * p, size_t n) { return std::vector<uint8_t>(p, p + n); }

std::string hex(const std::vector<uint8_t> & v)
{
    static const char * d = "0123456789abcdef";
    std::string s;
    for(uint8_t b : v) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

/* In-memory byte pipe pair: what A writes, B reads, and vice versa. Optional corruption. */
struct Pipe {
    std::deque<uint8_t> q;
};
struct PipeStream final : hal::ByteStream {
    Pipe * tx;
    Pipe * rx;
    int corrupt_next = -1;   // flip a bit in the Nth byte written
    PipeStream(Pipe * t, Pipe * r) : tx(t), rx(r) {}
    bool begin() override { return true; }
    size_t write(const uint8_t * d, size_t n) override
    {
        for(size_t i = 0; i < n; i++) {
            uint8_t b = d[i];
            if(corrupt_next == 0) b ^= 0x10;
            if(corrupt_next >= 0) corrupt_next--;
            tx->q.push_back(b);
        }
        return n;
    }
    size_t read(uint8_t * buf, size_t max) override
    {
        size_t n = 0;
        while(n < max && !rx->q.empty() && n < 7) {   // trickle: odd-sized reads
            buf[n++] = rx->q.front();
            rx->q.pop_front();
        }
        return n;
    }
};

}  // namespace

/* ---------------------------------- codec ---------------------------------- */

TEST(codec_state_golden_v1)
{
    State s;
    s.temp_c = 20.22f;
    s.temp_valid = true;
    s.setpoint_c[0] = 18.89f;
    s.setpoint_c[1] = 1.11f;
    s.mode = Mode::Crash;
    s.paused = true;
    s.action = Action::Cooling;
    s.celsius = false;
    std::strcpy(s.batch, "IPA");
    s.day = 260;
    uint8_t b[codec::kMaxEncoded];
    const size_t n = codec::encode(s, 0x1234, b, sizeof(b));
    /* ver 01, type 01, seq 3412 | e607 (2022) 6107 (1889) 6f00 (111) | 01 01 05 | 0401 | 03 'IPA'
     * If this changes, you broke wire compatibility with deployed boards: bump proto_ver instead. */
    CHECK_EQ(hex(bytes(b, n)), std::string("01013412" "e607" "6107" "6f00" "010105" "0401" "03495041"));
}

TEST(codec_command_and_history_golden_v1)
{
    uint8_t b[codec::kMaxEncoded];
    size_t n = codec::encode(Command::setSetpoint(Mode::Ferment, 18.5f), 1, b, sizeof(b));
    CHECK_EQ(hex(bytes(b, n)), std::string("010201000100" "3a07" "00"));
    HistoryChunk h;
    h.reset = true;
    h.minutes_per_sample = 30;
    h.n = 2;
    h.temps_c[0] = -1.0f;
    h.temps_c[1] = 2.5f;
    n = codec::encode(h, 2, b, sizeof(b));
    CHECK_EQ(hex(bytes(b, n)), std::string("01030200" "011e02" "9cff" "fa00"));
}

TEST(codec_round_trips)
{
    State s;
    s.temp_c = -3.25f;
    s.temp_valid = true;
    s.setpoint_c[0] = 19.0f;
    s.setpoint_c[1] = 0.5f;
    s.action = Action::Heating;
    s.celsius = true;
    std::strcpy(s.batch, "Fifteen chars!!");
    s.day = 7;
    uint8_t b[codec::kMaxEncoded];
    size_t n = codec::encode(s, 9, b, sizeof(b));
    codec::Message m;
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK(m.type == codec::MsgType::State);
    CHECK_EQ(m.seq, static_cast<uint16_t>(9));
    CHECK_NEAR(m.state.temp_c, -3.25, 1e-6);
    CHECK(m.state.temp_valid && m.state.celsius && !m.state.paused);
    CHECK(m.state.action == Action::Heating);
    CHECK_EQ(std::string(m.state.batch), std::string("Fifteen chars!!"));
    CHECK_EQ(m.state.day, static_cast<uint16_t>(7));

    n = codec::encode(Command::setMode(Mode::Crash, true), 3, b, sizeof(b));
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK(m.type == codec::MsgType::Command);
    CHECK(m.command.kind == Command::Kind::SetMode && m.command.mode == Mode::Crash && m.command.paused);
}

TEST(codec_rejects_bad_input_and_ignores_trailing_fields)
{
    uint8_t b[codec::kMaxEncoded];
    codec::Message m;
    size_t n = codec::encode(Command::setUnits(true), 0, b, sizeof(b));
    CHECK(codec::decode(b, 3, m) == codec::DecodeResult::TooShort);
    b[0] = 2;
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::BadVersion);
    b[0] = 1;
    b[1] = 99;
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::UnknownType);
    b[1] = 2;
    CHECK(codec::decode(b, n - 1, m) == codec::DecodeResult::BadPayload);
    /* a newer sender appended a field: v1 decoders must still accept the message */
    b[n] = 0xAB;
    CHECK(codec::decode(b, n + 1, m) == codec::DecodeResult::Ok);
    CHECK(m.command.celsius);
    /* too small an output buffer encodes nothing */
    CHECK_EQ(codec::encode(State{}, 0, b, 8), static_cast<size_t>(0));
}

/* --------------------------------- framing --------------------------------- */

TEST(crc32_known_vector)
{
    const char * s = "123456789";
    CHECK_EQ(framing::crc32(reinterpret_cast<const uint8_t *>(s), 9), 0xCBF43926u);
}

TEST(cobs_round_trips)
{
    std::vector<std::vector<uint8_t>> cases = { {}, { 0 }, { 0, 0 }, { 1, 2, 3 }, { 1, 0, 2, 0 } };
    std::vector<uint8_t> long_run(300);
    for(size_t i = 0; i < long_run.size(); i++) long_run[i] = static_cast<uint8_t>(i % 255 + 1);
    cases.push_back(long_run);
    std::vector<uint8_t> run254(254, 7);
    cases.push_back(run254);
    for(const auto & c : cases) {
        uint8_t enc[400], dec[400];
        const size_t e = framing::cobsEncode(c.data(), c.size(), enc, sizeof(enc));
        CHECK(e > 0);
        for(size_t i = 0; i < e; i++) CHECK(enc[i] != 0);
        const size_t d = framing::cobsDecode(enc, e, dec, sizeof(dec));
        CHECK_EQ(d, c.size());
        CHECK(std::memcmp(dec, c.data(), c.size()) == 0);
    }
}

TEST(stream_link_delivers_packets_and_drops_corrupt_ones)
{
    Pipe ab, ba;
    PipeStream a(&ab, &ba), b(&ba, &ab);
    StreamPacketLink la(a), lb(b);
    const uint8_t p1[] = { 1, 0, 2, 0, 0, 3 };
    const uint8_t p2[] = { 9, 9 };
    const uint8_t p3[] = { 4, 5, 6 };
    CHECK(la.send(p1, sizeof(p1)));
    a.corrupt_next = 2;
    CHECK(la.send(p2, sizeof(p2)));   // corrupted on the way
    CHECK(la.send(p3, sizeof(p3)));
    uint8_t buf[64];
    size_t n = lb.recv(buf, sizeof(buf));
    CHECK_EQ(n, sizeof(p1));
    CHECK(std::memcmp(buf, p1, sizeof(p1)) == 0);
    n = lb.recv(buf, sizeof(buf));
    CHECK_EQ(n, sizeof(p3));
    CHECK(std::memcmp(buf, p3, sizeof(p3)) == 0);
    CHECK_EQ(lb.recv(buf, sizeof(buf)), static_cast<size_t>(0));
    CHECK_EQ(lb.droppedFrames(), 1u);
}

/* --------------------------- remote, end to end ---------------------------- */

namespace {
struct NullOut final : hal::DigitalOut {
    void write(bool) override {}
};
struct ConstSensor final : hal::TempSensor {
    float c;
    explicit ConstSensor(float v) : c(v) {}
    bool begin() override { return true; }
    void update(uint32_t) override {}
    hal::TempReading latest() const override { hal::TempReading r; r.celsius = c; r.valid = true; return r; }
    const char * name() const override { return "const"; }
};
}  // namespace

TEST(remote_display_and_controller_over_uart)
{
    Pipe d2c, c2d;
    PipeStream ds(&d2c, &c2d), cs(&c2d, &d2c);
    StreamPacketLink dl(ds), cl(cs);
    RemoteControllerProxy display(dl, 3000);
    RemoteUiProxy uiside(cl);

    ConstSensor sensor(20.0f);
    DeadbandLaw law(0.2f);
    NullOut h, c;
    OutputGuard guard(&h, &c, {});
    Controller ctl(uiside, sensor, law, guard, nullptr, Controller::Config{});
    float hist[30];
    for(int i = 0; i < 30; i++) hist[i] = 15.0f + 0.1f * i;
    ctl.history().assign(hist, 30);

    uint32_t now = 1;
    display.service(now);   // first call asks for a full sync
    for(int i = 0; i < 5; i++) {
        ctl.tick(now);
        display.service(now);
        now += 20;
    }
    State s;
    CHECK(display.pollState(s));
    CHECK_NEAR(s.temp_c, 20.0, 0.01);
    CHECK(s.action == Action::Cooling);   // 20 C vs default 18.9 C
    HistoryChunk hc;
    int total = 0;
    bool first_reset = false;
    while(display.pollHistory(hc)) {   // boot sync + requested sync: count from the last reset
        if(total == 0) first_reset = hc.reset;
        if(hc.reset) total = 0;
        total += hc.n;
    }
    CHECK(first_reset);
    CHECK_EQ(total, 30);

    /* command from the display reaches the controller and comes back in State */
    display.send(Command::setSetpoint(Mode::Ferment, 21.0f));
    for(int i = 0; i < 3; i++) {
        ctl.tick(now);
        display.service(now);
        now += 20;
    }
    CHECK(display.pollState(s));
    CHECK_NEAR(s.setpoint_c[0], 21.0, 0.01);
    CHECK(s.action == Action::Heating);

    /* the link goes quiet (controller gone) -> display re-requests a sync after resync_ms */
    const size_t before = d2c.q.size();
    now += 3100;
    display.service(now);
    CHECK(d2c.q.size() > before);
}

TEST(remote_command_lost_on_the_wire_is_resent_without_snapping_back)
{
    Pipe d2c, c2d;
    PipeStream ds(&d2c, &c2d), cs(&c2d, &d2c);
    StreamPacketLink dl(ds), cl(cs);
    RemoteControllerProxy display(dl, 3000, 500, 15000);
    RemoteUiProxy uiside(cl);
    ConstSensor sensor(20.0f);
    DeadbandLaw law(0.2f);
    NullOut h, c;
    OutputGuard guard(&h, &c, {});
    Controller ctl(uiside, sensor, law, guard, nullptr, Controller::Config{});
    const float old_sp = Controller::Config{}.setpoint_c[0];

    uint32_t now = 1;
    State s;
    auto run = [&](int steps) {
        for(int i = 0; i < steps; i++) {
            display.service(now);
            ctl.tick(now);
            now += 20;
        }
    };
    run(5);
    while(display.pollState(s)) {}

    display.send(Command::setSetpoint(Mode::Ferment, 21.0f));
    d2c.q.clear();                      // the command never arrives
    CHECK_EQ(display.pendingCommands(), 1);
    ctl.evaluateNow(now);               // the controller keeps publishing its old setpoint
    run(1);
    CHECK(display.pollState(s));
    CHECK_NEAR(s.setpoint_c[0], 21.0, 0.01);   // UI keeps the user's value, no snap back
    CHECK_NEAR(ctl.state().setpoint_c[0], old_sp, 0.01);

    run(30);                            // 600 ms: past retry_ms, re-sent and applied
    CHECK_NEAR(ctl.state().setpoint_c[0], 21.0, 0.01);
    CHECK_EQ(display.pendingCommands(), 0);

    /* a command the controller ignores (bad mode) is dropped after give_up_ms */
    Command bad = Command::setMode(Mode::Ferment, true);
    bad.mode = static_cast<Mode>(7);
    display.send(bad);
    run(10);
    CHECK_EQ(display.pendingCommands(), 1);
    now += 15000;
    run(1);
    CHECK_EQ(display.pendingCommands(), 0);
}
