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
    s.wanted = Action::Heating;
    s.wait_s = 300;
    uint8_t b[codec::kMaxEncoded];
    const size_t n = codec::encode(s, 0x1234, b, sizeof(b));
    /* ver 01, type 01, seq 3412 | e607 (2022) 6107 (1889) 6f00 (111) | 01 01 05 | 0401 | 03 'IPA'
     * If this prefix changes, you broke wire compatibility with deployed boards: bump proto_ver
     * instead. Fields appended later (rule 5) follow it; pin each one as it is added. */
    const std::string v1_original = "01013412" "e607" "6107" "6f00" "010105" "0401" "03495041";
    const std::string appended = "02"      /* #9 wanted: heating */
                                 "2c01";   /* #10 wait_s: 300 */
    CHECK_EQ(hex(bytes(b, n)), v1_original + appended);
}

TEST(codec_state_from_an_older_controller_is_never_waiting)
{
    /* The golden bytes above without #9/#10, as a controller from before they existed sends. */
    const uint8_t old[] = { 0x01, 0x01, 0x34, 0x12, 0xe6, 0x07, 0x61, 0x07, 0x6f, 0x00,
                            0x01, 0x01, 0x05, 0x04, 0x01, 0x03, 0x49, 0x50, 0x41 };
    codec::Message m;
    CHECK(codec::decode(old, sizeof(old), m) == codec::DecodeResult::Ok);
    CHECK(m.state.action == Action::Cooling);
    CHECK(m.state.wanted == Action::Cooling);   // defaults to what it is doing
    CHECK_EQ(m.state.wait_s, static_cast<uint16_t>(0));
    CHECK_EQ(std::string(m.state.batch), std::string("IPA"));
}

TEST(codec_command_and_history_golden_v1)
{
    uint8_t b[codec::kMaxEncoded];
    size_t n = codec::encode(Command::setSetpoint(Mode::Ferment, 18.5f), 1, b, sizeof(b));
    CHECK_EQ(hex(bytes(b, n)), std::string("010201000100" "3a07" "00"   /* v1 as deployed */
                                           "00" "00000000"             /* #5 tuning, #6 value (appended) */
                                           "00" "0000" "0000"          /* #7 step, #8 ramp_h, #9 hold_h */
                                           "00"));                     /* #10 name: empty (appended) */
    HistoryChunk h;
    h.reset = true;
    h.ms_per_sample = 30u * 60u * 1000u;
    h.n = 2;
    h.temps_c[0] = -1.0f;
    h.temps_c[1] = 2.5f;
    n = codec::encode(h, 2, b, sizeof(b));
    /* original v1 bytes, then (appended) #5 seconds_per_sample 1800 = 0x0708,
     * then #6 ms_per_sample 1800000 = 0x001b7740 */
    CHECK_EQ(hex(bytes(b, n)), std::string("01030200" "011e02" "9cff" "fa00") + "0807" + "40771b00");
}

TEST(codec_history_ms_per_sample)
{
    uint8_t b[codec::kMaxEncoded];
    codec::Message m;
    HistoryChunk h;
    h.reset = true;
    h.ms_per_sample = 200;
    h.n = 1;
    h.temps_c[0] = 18.0f;
    size_t n = codec::encode(h, 0, b, sizeof(b));
    CHECK_EQ(b[5], static_cast<uint8_t>(0));                // #2 minutes: under a minute rounds to 0
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK_EQ(b[9], static_cast<uint8_t>(1));                // #5 seconds: never 0 for a set interval
    CHECK_EQ(m.history.ms_per_sample, 200u);
    /* an older controller sends no #5: whole minutes it is */
    const uint8_t old[] = { 0x01, 0x03, 0x02, 0x00, 0x01, 0x01, 0x01, 0x08, 0x07 };
    CHECK(codec::decode(old, sizeof(old), m) == codec::DecodeResult::Ok);
    CHECK_EQ(m.history.ms_per_sample, 60000u);
    /* one that predates #6 sends whole seconds */
    const uint8_t secs[] = { 0x01, 0x03, 0x02, 0x00, 0x01, 0x00, 0x01, 0x08, 0x07, 0x02, 0x00 };
    CHECK(codec::decode(secs, sizeof(secs), m) == codec::DecodeResult::Ok);
    CHECK_EQ(m.history.ms_per_sample, 2000u);
    CHECK_EQ(m.history.n, static_cast<uint8_t>(1));
    CHECK_NEAR(m.history.temps_c[0], 18.0, 1e-6);
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
    s.wanted = Action::Cooling;
    s.wait_s = 65535;
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
    CHECK(m.state.wanted == Action::Cooling);
    CHECK_EQ(m.state.wait_s, static_cast<uint16_t>(65535));

    n = codec::encode(Command::setMode(Mode::Crash, true), 3, b, sizeof(b));
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK(m.type == codec::MsgType::Command);
    CHECK(m.command.kind == Command::Kind::SetMode && m.command.mode == Mode::Crash && m.command.paused);

    n = codec::encode(Command::startDayCount(), 4, b, sizeof(b));
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK(m.command.kind == Command::Kind::StartDayCount);
    b[4] = 12;  // a kind this version doesn't know is refused, not guessed at
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::BadPayload);

    /* SetBatch: start + name, cut to 15 bytes without splitting a character */
    n = codec::encode(Command::setBatch(1795582800u, "B\xC3\xA4rktoberfest GF"), 6, b, sizeof(b));
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK(m.command.kind == Command::Kind::SetBatch);
    CHECK_EQ(static_cast<uint32_t>(m.command.value), 1795582800u);
    CHECK(std::string(m.command.name) == "B\xC3\xA4rktoberfest ");   // 15 bytes: "ä" is two
    CHECK(std::string(Command::setBatch(0, "12345678901234\xC3\xA4").name) == "12345678901234");   // never half an "ä"
    CHECK(codec::decode(b, n - 1, m) == codec::DecodeResult::BadPayload);   // name cut short
    /* an older display's command (no #10) still decodes, with no name */
    n = codec::encode(Command::setUnits(true), 7, b, sizeof(b));
    CHECK(codec::decode(b, n - 1, m) == codec::DecodeResult::Ok);
    CHECK(m.command.name[0] == '\0');

    /* SetClock: Unix seconds, still right past 2038 (the i32 field carries a u32) */
    for(const uint32_t t : { 1795132800u, 0x80000010u }) {
        n = codec::encode(Command::setClock(t), 5, b, sizeof(b));
        CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
        CHECK(m.command.kind == Command::Kind::SetClock);
        CHECK_EQ(static_cast<uint32_t>(m.command.value), t);
    }
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
    CHECK(codec::decode(b, 8, m) == codec::DecodeResult::BadPayload);   // the original fields cut short
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

TEST(codec_tuning_round_trips_and_older_messages_still_decode)
{
    uint8_t b[codec::kMaxEncoded];
    codec::Message m;
    size_t n = codec::encode(Command::setTuning(Tuning::CoolDelay, 300), 5, b, sizeof(b));
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK(m.command.kind == Command::Kind::SetTuning);
    CHECK(m.command.tuning == Tuning::CoolDelay);
    CHECK_EQ(m.command.value, 300);
    n = codec::encode(Command::setTuning(Tuning::CrashDefault, -150), 6, b, sizeof(b));
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK_EQ(m.command.value, -150);
    CHECK(codec::decode(b, 9, m) == codec::DecodeResult::BadPayload);   // set_tuning without its fields
    b[9] = 9;   // a setting this version doesn't know
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::BadPayload);

    /* a command from an older display (no #5/#6) still decodes */
    n = codec::encode(Command::setUnits(true), 7, b, sizeof(b));
    CHECK(codec::decode(b, 9, m) == codec::DecodeResult::Ok);
    CHECK(m.command.celsius);

    Settings st;
    st.heat_diff_c = 0.28f;
    st.cool_diff_c = 0.56f;
    st.heat_delay_s = 120;
    st.cool_delay_s = 600;
    st.crash_default_c = -1.5f;
    n = codec::encode(st, 8, b, sizeof(b));
    /* ver 01, type 04 (Settings), seq 0800 | 1c00 (28) 3800 (56) 7800 (120) 5802 (600) 6aff (-150) */
    CHECK_EQ(hex(bytes(b, n)), std::string("01040800" "1c00" "3800" "7800" "5802" "6aff"
                                           "00" "00" "00" "00"));   /* #6..#9: constant, step 0, no flags, no steps */
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK(m.type == codec::MsgType::Settings);
    CHECK_NEAR(m.settings.heat_diff_c, 0.28, 1e-6);
    CHECK_NEAR(m.settings.cool_diff_c, 0.56, 1e-6);
    CHECK_EQ(m.settings.heat_delay_s, static_cast<uint16_t>(120));
    CHECK_EQ(m.settings.cool_delay_s, static_cast<uint16_t>(600));
    CHECK_NEAR(m.settings.crash_default_c, -1.5, 1e-6);
    CHECK(codec::decode(b, 13, m) == codec::DecodeResult::BadPayload);   // #1..#5 cut short
}

TEST(remote_tuning_lost_on_the_wire_is_resent_and_shown_meanwhile)
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

    uint32_t now = 1;
    auto run = [&](int steps) {
        for(int i = 0; i < steps; i++) {
            display.service(now);
            ctl.tick(now);
            now += 20;
        }
    };
    run(5);
    Settings st;
    CHECK(display.pollSettings(st));   // sent at start (and on the sync request)
    CHECK_EQ(st.cool_delay_s, static_cast<uint16_t>(180));
    while(display.pollSettings(st)) {}

    display.send(Command::setTuning(Tuning::CoolDelay, 420));
    for(int i = 0; i < 260; i++) {      // 5.2 s with that direction of the cable dead: every
        display.service(now);           // retry is lost too, and the controller re-sends its old
        d2c.q.clear();                  // Settings once in that time
        ctl.tick(now);
        now += 20;
    }
    CHECK_EQ(display.pendingCommands(), 1);
    CHECK(display.pollSettings(st));
    CHECK_EQ(st.cool_delay_s, static_cast<uint16_t>(420));   // the display keeps the new value
    CHECK_EQ(ctl.settings().cool_delay_s, static_cast<uint16_t>(180));
    run(30);                            // re-sent after retry_ms, applied, confirmed by Settings
    CHECK_EQ(ctl.settings().cool_delay_s, static_cast<uint16_t>(420));
    CHECK_EQ(display.pendingCommands(), 0);
}

TEST(codec_settings_carry_the_schedule)
{
    uint8_t b[codec::kMaxEncoded];
    codec::Message m;
    Settings st;
    st.profile = Profile::Schedule;
    st.step = 1;
    st.override_on = true;
    st.nsteps = Settings::kMaxSteps;   // the longest: still fits a frame
    for(int i = 0; i < st.nsteps; i++) st.steps[i] = { 18.0f + static_cast<float>(i), static_cast<uint16_t>(i * 24), 72 };
    size_t n = codec::encode(st, 1, b, sizeof(b));
    CHECK(n > 0 && n < 100);
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK(m.settings.profile == Profile::Schedule && m.settings.step == 1 && m.settings.override_on);
    CHECK(!m.settings.simulated);
    CHECK_EQ(m.settings.nsteps, 8);
    CHECK_NEAR(m.settings.steps[7].temp_c, 25.0, 1e-6);
    CHECK_EQ(m.settings.steps[7].ramp_h, 168);
    CHECK_EQ(m.settings.steps[3].hold_h, 72);
    CHECK(codec::decode(b, n - 1, m) == codec::DecodeResult::BadPayload);   // steps cut short

    /* a demo build: bit1 of #8, next to the override */
    st.simulated = true;
    st.override_on = false;
    n = codec::encode(st, 3, b, sizeof(b));
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK(m.settings.simulated && !m.settings.override_on);

    n = codec::encode(Command::setStep(2, 20.5f, 24, 48), 2, b, sizeof(b));
    CHECK(codec::decode(b, n, m) == codec::DecodeResult::Ok);
    CHECK(m.command.kind == Command::Kind::SetStep && m.command.step == 2);
    CHECK_NEAR(m.command.setpoint_c, 20.5, 1e-6);
    CHECK_EQ(m.command.ramp_h, 24);
    CHECK_EQ(m.command.hold_h, 48);
    CHECK(codec::decode(b, 14, m) == codec::DecodeResult::BadPayload);   // set_step without #7..#9
}
