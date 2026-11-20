/* Control laws, OutputGuard, Controller (commands, persistence, history, fail-safe). */
#include "brew/controller.h"
#include "brew/fake_plant.h"
#include "brew/local_bus.h"
#include "minitest.h"

#include <map>
#include <string>

using namespace brew;

namespace {

struct FakeOut final : hal::DigitalOut {
    bool on = false;
    int writes = 0;
    void write(bool v) override { on = v; writes++; }
};

struct FakeSensor final : hal::TempSensor {
    hal::TempReading r;
    bool begin() override { return true; }
    void update(uint32_t) override {}
    hal::TempReading latest() const override { return r; }
    const char * name() const override { return "fake"; }
    void set(float c) { r.celsius = c; r.valid = true; }
};

struct MemStore final : hal::SettingsStore {
    std::map<std::string, int32_t> i;
    std::map<std::string, std::string> s;
    int commits = 0;
    bool getI32(const char * k, int32_t & o) override { auto it = i.find(k); if(it == i.end()) return false; o = it->second; return true; }
    bool setI32(const char * k, int32_t v) override { i[k] = v; return true; }
    bool getStr(const char * k, char * o, size_t m) override
    {
        auto it = s.find(k);
        if(it == s.end()) return false;
        std::snprintf(o, m, "%s", it->second.c_str());
        return true;
    }
    bool setStr(const char * k, const char * v) override { s[k] = v; return true; }
    bool commit() override { commits++; return true; }
};

/* Captures what the controller publishes. */
struct CapturePort final : ControlPort {
    SpscRing<Command, 16> in;
    State last;
    int states = 0;
    std::vector<HistoryChunk> hist;
    bool pollCommand(Command & c) override { return in.pop(c); }
    void publish(const State & s) override { last = s; states++; }
    void publish(const HistoryChunk & h) override { hist.push_back(h); }
};

}  // namespace

TEST(deadband_law)
{
    DeadbandLaw l(0.5f);
    CHECK(l.step(20.6f, 20.0f, 0) == Action::Cooling);
    CHECK(l.step(20.4f, 20.0f, 0) == Action::Idle);
    CHECK(l.step(19.4f, 20.0f, 0) == Action::Heating);
}

TEST(hysteresis_law_runs_to_setpoint)
{
    HysteresisLaw l(0.5f);
    CHECK(l.step(20.6f, 20.0f, 0) == Action::Cooling);
    CHECK(l.step(20.2f, 20.0f, 0) == Action::Cooling);   // still above setpoint: keep cooling
    CHECK(l.step(20.0f, 20.0f, 0) == Action::Idle);
    CHECK(l.step(20.4f, 20.0f, 0) == Action::Idle);      // inside the band: stay idle
}

TEST(guard_interlock_and_passthrough)
{
    FakeOut h, c;
    OutputGuard g(&h, &c, {});
    CHECK(g.apply(Action::Heating, 0) == Action::Heating);
    CHECK(h.on && !c.on);
    CHECK(g.apply(Action::Cooling, 10) == Action::Cooling);   // no delays configured: direct switch
    CHECK(!h.on && c.on);
    CHECK(g.apply(Action::Idle, 20) == Action::Idle);
    CHECK(!h.on && !c.on);
}

TEST(guard_min_on_min_off_changeover)
{
    FakeOut h, c;
    OutputGuard::Config cfg;
    cfg.cool.min_on_ms = 1000;
    cfg.cool.min_off_ms = 5000;   // compressor delay
    cfg.heat.min_on_ms = 1000;    // heat wrap: no rest needed
    cfg.changeover_ms = 2000;
    OutputGuard g(&h, &c, cfg);
    CHECK(g.apply(Action::Cooling, 0) == Action::Idle);        // power-up counts as "just stopped"
    CHECK_EQ(g.blockedFor(Action::Cooling, 0), 5000u);
    CHECK(g.apply(Action::Cooling, 4999) == Action::Idle);
    CHECK(g.apply(Action::Cooling, 5000) == Action::Cooling);
    CHECK(g.apply(Action::Idle, 5500) == Action::Cooling);     // cool min_on not reached
    CHECK(g.apply(Action::Idle, 6000) == Action::Idle);        // cooling stopped at 6000
    CHECK(g.apply(Action::Cooling, 7000) == Action::Idle);     // cool min_off: rest until 11000
    CHECK(g.apply(Action::Heating, 7500) == Action::Idle);     // changeover until 8000
    CHECK(g.apply(Action::Heating, 8000) == Action::Heating);  // heat has no min_off
    CHECK(g.apply(Action::Cooling, 8500) == Action::Heating);  // heat min_on until 9000
    CHECK(g.apply(Action::Cooling, 9000) == Action::Idle);     // heat -> cool passes through idle
    CHECK(g.apply(Action::Cooling, 10000) == Action::Idle);    // changeover since heat stopped (9000)
    CHECK_EQ(g.blockedFor(Action::Cooling, 10000), 1000u);
    CHECK(g.apply(Action::Cooling, 11000) == Action::Cooling);
    CHECK(!h.on && c.on);
}

TEST(guard_heat_starts_at_power_up_without_a_heat_rest)
{
    FakeOut h, c;
    OutputGuard g(&h, &c, ChamberDefaults::guard());
    CHECK(g.apply(Action::Heating, 0) == Action::Heating);
    CHECK(h.on);
}

TEST(hysteresis_law_separate_differentials)
{
    HysteresisLaw l(1.0f, 0.25f);   // heat differential 1, cool differential 0.25
    CHECK(l.step(19.5f, 20.0f, 0) == Action::Idle);      // within the heat differential
    CHECK(l.step(18.9f, 20.0f, 0) == Action::Heating);
    CHECK(l.step(19.9f, 20.0f, 0) == Action::Heating);   // runs up to the setpoint
    CHECK(l.step(20.0f, 20.0f, 0) == Action::Idle);
    CHECK(l.step(20.3f, 20.0f, 0) == Action::Cooling);   // cool differential is tighter
}

TEST(controller_commands_persist_and_reevaluate)
{
    CapturePort port;
    FakeSensor s;
    s.set(f_to_c(68.4f));
    DeadbandLaw law(0.3f * 5.0f / 9.0f);
    FakeOut h, c;
    OutputGuard guard(&h, &c, {});
    MemStore store;
    Controller ctl(port, s, law, guard, &store, Controller::Config{});
    ctl.begin(0);
    ctl.tick(0);
    CHECK(port.last.action == Action::Cooling);   // 68.4 F vs default 66 F
    CHECK(c.on);

    port.in.push(Command::setSetpoint(Mode::Ferment, f_to_c(70.0f)));
    ctl.tick(20);
    CHECK_NEAR(c_to_f(port.last.setpoint_c[0]), 70.0, 0.01);
    CHECK(port.last.action == Action::Heating);
    CHECK_EQ(store.i["sp0_cc"], 2111);   // 70 F = 21.11 C

    port.in.push(Command::setMode(Mode::Crash, true));
    ctl.tick(40);
    CHECK(port.last.mode == Mode::Crash);
    CHECK(port.last.paused);
    CHECK(port.last.action == Action::Idle);   // paused: everything off
    CHECK(!h.on && !c.on);

    port.in.push(Command::setUnits(true));
    ctl.tick(60);
    CHECK(port.last.celsius);

    /* a new controller with the same store comes back in the same state */
    CapturePort port2;
    Controller ctl2(port2, s, law, guard, &store, Controller::Config{});
    ctl2.begin(0);
    ctl2.tick(0);
    CHECK(port2.last.mode == Mode::Crash);
    CHECK(port2.last.paused);
    CHECK(port2.last.celsius);
    CHECK_NEAR(c_to_f(port2.last.setpoint_c[0]), 70.0, 0.01);
}

TEST(controller_publishes_what_it_wants_while_the_guard_holds_it_off)
{
    CapturePort port;
    FakeSensor s;
    s.set(f_to_c(68.6f));
    DeadbandLaw law(0.3f * 5.0f / 9.0f);
    FakeOut h, c;
    OutputGuard::Config g;
    g.cool.min_off_ms = 300000;   // 5 min compressor delay, counted from power-up
    OutputGuard guard(&h, &c, g);
    Controller ctl(port, s, law, guard, nullptr, Controller::Config{});
    ctl.begin(0);
    ctl.tick(0);
    CHECK(port.last.action == Action::Idle);      // held off...
    CHECK(port.last.wanted == Action::Cooling);   // ...but it wants to cool
    CHECK_EQ(port.last.wait_s, static_cast<uint16_t>(300));
    ctl.tick(60500);
    CHECK_EQ(port.last.wait_s, static_cast<uint16_t>(240));   // rounded up: 239.5 s
    ctl.tick(300000);
    CHECK(port.last.action == Action::Cooling && c.on);
    CHECK(port.last.wanted == Action::Cooling);
    CHECK_EQ(port.last.wait_s, static_cast<uint16_t>(0));
}

TEST(controller_fails_safe_without_reading)
{
    CapturePort port;
    FakeSensor s;   // never valid
    DeadbandLaw law(0.1f);
    FakeOut h, c;
    OutputGuard guard(&h, &c, {});
    Controller ctl(port, s, law, guard, nullptr, Controller::Config{});
    ctl.tick(0);
    CHECK(!port.last.temp_valid);
    CHECK(port.last.action == Action::Idle);
    CHECK(!h.on && !c.on);
}

TEST(controller_history_sync_and_sampling)
{
    CapturePort port;
    FakeSensor s;
    s.set(18.0f);
    DeadbandLaw law(0.1f);
    OutputGuard guard(nullptr, nullptr, {});
    Controller::Config cfg;
    cfg.history_minutes = 1;
    Controller ctl(port, s, law, guard, nullptr, cfg);
    float h[50];
    for(int i = 0; i < 50; i++) h[i] = 10.0f + i;
    ctl.history().assign(h, 50);
    ctl.begin(0);
    CHECK_EQ(port.hist.size(), static_cast<size_t>(3));   // begin() publishes the history too
    CHECK(port.hist[0].reset);
    port.hist.clear();

    port.in.push(Command::requestSync());
    ctl.tick(0);
    CHECK_EQ(port.hist.size(), static_cast<size_t>(3));   // 24 + 24 + 2
    CHECK(port.hist[0].reset);
    CHECK_EQ(port.hist[0].minutes_per_sample, static_cast<uint8_t>(1));
    CHECK(!port.hist[1].reset);
    CHECK(port.hist[0].more && port.hist[1].more && !port.hist[2].more);
    CHECK_EQ(port.hist[2].n, static_cast<uint8_t>(2));
    CHECK_NEAR(port.hist[2].temps_c[1], 59.0, 1e-6);

    port.hist.clear();
    ctl.tick(60000);   // one sample per minute
    CHECK_EQ(port.hist.size(), static_cast<size_t>(1));
    CHECK_EQ(port.hist[0].n, static_cast<uint8_t>(1));
    CHECK_NEAR(port.hist[0].temps_c[0], 18.0, 1e-6);
    CHECK_EQ(ctl.history().size(), 51);
}

TEST(controller_history_sample_is_the_mean_reading)
{
    CapturePort port;
    FakeSensor s;
    DeadbandLaw law(0.1f);
    OutputGuard guard(nullptr, nullptr, {});
    Controller::Config cfg;
    cfg.history_minutes = 1;
    cfg.period_ms = 1000;
    Controller ctl(port, s, law, guard, nullptr, cfg);
    ctl.begin(0);
    port.hist.clear();
    for(uint32_t t = 0; t <= 60000; t += 1000) {   // 30 s at 18 C, then 31 s at 20 C
        s.set(t < 30000 ? 18.0f : 20.0f);
        ctl.tick(t);
    }
    CHECK_EQ(port.hist.size(), static_cast<size_t>(1));
    CHECK_NEAR(port.hist[0].temps_c[0], (30 * 18.0 + 31 * 20.0) / 61.0, 1e-3);
    CHECK_EQ(ctl.history().size(), 1);   // starts empty: no made-up samples
}

TEST(fake_plant_heat_balance)
{
    FakePlant::Config cfg;
    cfg.start_c = 20.0f;
    cfg.ambient_c = 20.0f;
    cfg.volume_l = 10.0f;      // 41.8 kJ/K
    cfg.heat_w = 41.8f;        // 1 K per 1000 s, before losses
    cfg.loss_w_per_k = 0.0f;
    cfg.heat_lag_s = 0.0f;
    cfg.quantum_c = 0.0f;
    FakePlant p(cfg);
    p.update(0);
    p.heat().write(true);
    p.update(1000000);        // 1000 s
    CHECK_NEAR(p.actual(), 21.0, 0.01);

    /* with losses it settles at ambient + P/UA */
    cfg.loss_w_per_k = 4.18f;  // equilibrium 10 K above ambient, time constant 10000 s
    p.reconfigure(cfg);
    p.update(1000000 + 200000000u);
    CHECK_NEAR(p.actual(), 30.0, 0.05);

    /* a lagging heater keeps delivering after its relay opens */
    cfg.heat_lag_s = 300.0f;
    FakePlant q(cfg);
    q.update(0);
    q.heat().write(true);
    q.update(600000);
    CHECK(q.heatPowerW() > 0.8f * cfg.heat_w);
    q.heat().write(false);
    q.update(660000);
    CHECK(q.heatPowerW() > 0.6f * cfg.heat_w);   // still warm a minute later
    const float t0 = q.actual();
    q.update(720000);
    CHECK(q.actual() > t0);                       // ...and still warming the beer
}

TEST(local_bus_mailbox_latest_wins)
{
    LocalBus bus;
    State s;
    CHECK(!bus.pollState(s));
    State a; a.day = 1;
    State b; b.day = 2;
    bus.publish(a);
    bus.publish(b);
    CHECK(bus.pollState(s));
    CHECK_EQ(s.day, static_cast<uint16_t>(2));
    CHECK(!bus.pollState(s));   // nothing newer
    bus.send(Command::setUnits(true));
    Command c;
    CHECK(bus.pollCommand(c));
    CHECK(c.kind == Command::Kind::SetUnits && c.celsius);
    CHECK(!bus.pollCommand(c));
}
