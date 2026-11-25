/* Control laws, OutputGuard, Controller (commands, persistence, history, fail-safe). */
#include "brew/controller.h"
#include "brew/fake_plant.h"
#include "brew/local_bus.h"
#include "minitest.h"

#include <map>
#include <string>
#include <vector>

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
    size_t room = static_cast<size_t>(-1);   // queue size: publish fails when hist holds this many
    bool pollCommand(Command & c) override { return in.pop(c); }
    void publish(const State & s) override { last = s; states++; }
    Settings settings;
    int settings_n = 0;
    void publish(const Settings & s) override { settings = s; settings_n++; }
    bool publish(const HistoryChunk & h) override
    {
        if(hist.size() >= room) return false;
        hist.push_back(h);
        return true;
    }
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
    cfg.cool.min_off_ms = 5000;   // cooling restart delay
    cfg.heat.min_on_ms = 1000;    // heat: no restart delay here
    cfg.changeover_ms = 2000;
    OutputGuard g(&h, &c, cfg);
    CHECK_EQ(g.blockedFor(Action::Cooling, 0), 0u);           // nothing counted from power-up
    CHECK(g.apply(Action::Cooling, 0) == Action::Cooling);
    CHECK(g.apply(Action::Idle, 500) == Action::Cooling);      // cool min_on not reached
    CHECK(g.apply(Action::Idle, 1000) == Action::Idle);        // cooling stopped at 1000
    CHECK(g.apply(Action::Cooling, 2000) == Action::Idle);     // restart delay until 6000
    CHECK_EQ(g.blockedFor(Action::Cooling, 2000), 4000u);
    CHECK(g.apply(Action::Heating, 2500) == Action::Idle);     // changeover until 3000
    CHECK(g.apply(Action::Heating, 3000) == Action::Heating);  // heat has no min_off
    CHECK(g.apply(Action::Cooling, 3500) == Action::Heating);  // heat min_on until 4000
    CHECK(g.apply(Action::Cooling, 4000) == Action::Idle);     // heat -> cool passes through idle
    CHECK(g.apply(Action::Cooling, 5000) == Action::Idle);     // changeover since heat stopped (4000)
    CHECK_EQ(g.blockedFor(Action::Cooling, 5000), 1000u);      // ...and cooling's restart delay
    CHECK(g.apply(Action::Cooling, 6000) == Action::Cooling);
    CHECK(!h.on && c.on);
}

TEST(guard_defaults_start_either_relay_at_power_up)
{
    FakeOut h, c;
    OutputGuard g(&h, &c, ChamberDefaults::guard());
    CHECK(g.apply(Action::Cooling, 0) == Action::Cooling);
    CHECK(c.on);
    OutputGuard g2(&h, &c, ChamberDefaults::guard());
    CHECK(g2.apply(Action::Heating, 0) == Action::Heating);
    CHECK(h.on);
}

TEST(guard_defaults_restart_delay_per_relay)
{
    FakeOut h, c;
    OutputGuard g(&h, &c, ChamberDefaults::guard());
    CHECK(g.apply(Action::Heating, 0) == Action::Heating);
    CHECK(g.apply(Action::Idle, 10000) == Action::Idle);
    CHECK(g.apply(Action::Heating, 20000) == Action::Idle);       // heat rests until 190000
    CHECK_EQ(g.blockedFor(Action::Heating, 20000), 170000u);
    CHECK(g.apply(Action::Cooling, 30000) == Action::Cooling);   // cooling's own delay: not running yet
    CHECK(g.apply(Action::Heating, 40000) == Action::Idle);      // no gap: cooling stops, heat still rests
    CHECK(!h.on && !c.on);
    CHECK(g.apply(Action::Heating, 190000) == Action::Heating);
    CHECK(h.on && !c.on);
}

TEST(guard_stops_heating_when_cooling_is_wanted_but_still_resting)
{
    /* No changeover gap: heat -> cool is direct when cooling may start, but while cooling's
     * restart delay runs the heater must still switch off. */
    FakeOut h, c;
    OutputGuard g(&h, &c, ChamberDefaults::guard());
    CHECK(g.apply(Action::Cooling, 0) == Action::Cooling);
    CHECK(g.apply(Action::Heating, 10000) == Action::Heating);   // cooling stopped at 10000
    CHECK(g.apply(Action::Cooling, 30000) == Action::Idle);      // cooling rests until 190000
    CHECK(!h.on && !c.on);
    CHECK_EQ(g.blockedFor(Action::Cooling, 30000), 160000u);
    CHECK(g.apply(Action::Cooling, 190000) == Action::Cooling);
    CHECK(!h.on && c.on);
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

/* Advances a controller in 1 s ticks: the day count only needs seconds. */
static void run_s(Controller & ctl, uint32_t & now, uint32_t seconds)
{
    for(uint32_t i = 0; i < seconds; i++) ctl.tick(now += 1000);
}

TEST(controller_manual_day_count_survives_restarts_and_resets)
{
    CapturePort port;
    FakeSensor s;
    s.set(f_to_c(66.0f));
    DeadbandLaw law(0.3f * 5.0f / 9.0f);
    FakeOut h, c;
    OutputGuard guard(&h, &c, {});
    MemStore store;
    Controller::Config cfg;
    cfg.clock_save_s = 600;
    constexpr uint32_t kDay = 24u * 3600u;

    uint32_t now = 0;
    Controller ctl(port, s, law, guard, &store, cfg);
    ctl.begin(now);
    ctl.tick(now);
    CHECK_EQ(std::string(port.last.batch), std::string("Manual"));
    CHECK_EQ(port.last.day, static_cast<uint16_t>(1));    // first start: Day 1 begins now
    CHECK_EQ(store.i["day0_s"], 0);

    run_s(ctl, now, kDay - 1);
    CHECK_EQ(port.last.day, static_cast<uint16_t>(1));
    run_s(ctl, now, 2);
    CHECK_EQ(port.last.day, static_cast<uint16_t>(2));
    run_s(ctl, now, 3 * kDay);
    CHECK_EQ(port.last.day, static_cast<uint16_t>(5));
    /* the clock is saved every 10 min, so a power cut loses at most that */
    const int32_t saved = store.i["clk_s"];
    CHECK(static_cast<uint32_t>(saved) <= ctl.clockS() && ctl.clockS() - static_cast<uint32_t>(saved) < 600u);

    /* power cut: a new controller (uptime restarts at 0) picks up the saved clock and day */
    CapturePort port2;
    uint32_t now2 = 0;
    Controller ctl2(port2, s, law, guard, &store, cfg);
    ctl2.begin(now2);
    ctl2.tick(now2);
    CHECK_EQ(port2.last.day, static_cast<uint16_t>(5));
    CHECK_EQ(ctl2.dayStartS(), 0u);

    /* reset: Day 1 starts now, and that's saved too */
    run_s(ctl2, now2, 2 * kDay);
    CHECK_EQ(port2.last.day, static_cast<uint16_t>(7));
    port2.in.push(Command::startDayCount());
    ctl2.tick(now2 += 20);
    CHECK_EQ(port2.last.day, static_cast<uint16_t>(1));
    CHECK_EQ(static_cast<uint32_t>(store.i["day0_s"]), ctl2.clockS());
    run_s(ctl2, now2, kDay);
    CHECK_EQ(port2.last.day, static_cast<uint16_t>(2));

    /* ticks keep counting across the 49-day millis wrap */
    CapturePort port3;
    uint32_t now3 = 0xFFFFFFFFu - 1500u;
    Controller ctl3(port3, s, law, guard, &store, cfg);
    ctl3.begin(now3);
    const uint32_t c0 = ctl3.clockS();
    run_s(ctl3, now3, 5);
    CHECK_EQ(ctl3.clockS() - c0, 5u);
}

TEST(controller_day_count_switches_to_internet_time_and_counts_time_unplugged)
{
    CapturePort port;
    FakeSensor s;
    s.set(f_to_c(66.0f));
    DeadbandLaw law(0.3f * 5.0f / 9.0f);
    FakeOut h, c;
    OutputGuard guard(&h, &c, {});
    MemStore store;
    Controller::Config cfg;
    cfg.clock_save_s = 600;
    constexpr uint32_t kDay = 24u * 3600u;
    constexpr uint32_t kUnix = 1795132800u;   // 2026-11-20 00:00 UTC

    uint32_t now = 0;
    Controller ctl(port, s, law, guard, &store, cfg);
    ctl.begin(now);
    /* a two-step schedule started before there was WiFi: 20 C for 2 days, then 22 C */
    port.in.push(Command::setStep(0, 20.0f, 0, 48));
    port.in.push(Command::setStep(1, 22.0f, 0, 48));
    port.in.push(Command::setProfile(Profile::Schedule));
    ctl.tick(now += 20);
    run_s(ctl, now, kDay + 3600);                        // Day 2, an hour into it
    CHECK_EQ(port.last.day, static_cast<uint16_t>(2));
    CHECK(!ctl.clockIsUnix());

    /* a time that isn't set yet (1970) changes nothing */
    port.in.push(Command::setClock(5));
    ctl.tick(now += 20);
    CHECK(!ctl.clockIsUnix());

    /* the first internet time: the clock becomes Unix time, and Day 1 and the schedule move
     * with it, so the day, the step and the target stay as they were */
    const uint32_t before = ctl.clockS();
    port.in.push(Command::setClock(kUnix));
    ctl.tick(now += 20);
    CHECK(ctl.clockIsUnix());
    CHECK_EQ(ctl.clockS(), kUnix);
    CHECK_EQ(ctl.dayStartS(), kUnix - (before - 0u));     // Day 1 began `before` seconds ago
    CHECK_EQ(port.last.day, static_cast<uint16_t>(2));
    run_s(ctl, now, 2);
    CHECK_EQ(port.settings.step, 0);
    CHECK_NEAR(port.last.setpoint_c[0], 20.0, 1e-3);
    CHECK_EQ(store.i["clk_unix"], 1);
    CHECK_EQ(static_cast<uint32_t>(store.i["clk_s"]), kUnix);   // saved right away

    /* small drift isn't written to flash; the regular save catches it */
    const uint32_t saved = static_cast<uint32_t>(store.i["clk_s"]);
    port.in.push(Command::setClock(ctl.clockS() + 3));
    ctl.tick(now += 20);
    CHECK_EQ(static_cast<uint32_t>(store.i["clk_s"]), saved);

    /* unplugged for a day and a half: after the restart it counts from the saved clock until
     * the time arrives, then catches up, and the schedule is on its second step */
    const uint32_t unplugged_at = ctl.clockS();
    CapturePort port2;
    uint32_t now2 = 0;
    Controller ctl2(port2, s, law, guard, &store, cfg);
    ctl2.begin(now2);
    ctl2.tick(now2);
    CHECK(ctl2.clockIsUnix());
    CHECK_EQ(port2.last.day, static_cast<uint16_t>(2));
    port2.in.push(Command::setClock(unplugged_at + kDay + kDay / 2));
    ctl2.tick(now2 += 20);
    run_s(ctl2, now2, 2);
    CHECK_EQ(port2.last.day, static_cast<uint16_t>(3));
    CHECK_EQ(port2.settings.step, 1);
    CHECK_NEAR(port2.last.setpoint_c[0], 22.0, 1e-3);

    /* a clock that goes back before Day 1 doesn't make the day count wrap */
    const uint32_t back = ctl2.dayStartS() - 100;
    port2.in.push(Command::setClock(back));
    ctl2.tick(now2 += 20);
    CHECK_EQ(ctl2.dayStartS(), back);
    run_s(ctl2, now2, 1);
    CHECK_EQ(port2.last.day, static_cast<uint16_t>(1));

    /* restarting the day count saves Unix time, and keeps it after a restart */
    port2.in.push(Command::setClock(kUnix + 10 * kDay));
    port2.in.push(Command::startDayCount());
    ctl2.tick(now2 += 20);
    CHECK_EQ(static_cast<uint32_t>(store.i["day0_s"]), kUnix + 10 * kDay);
}

TEST(controller_settings_change_the_law_and_guard_and_are_kept)
{
    CapturePort port;
    FakeSensor s;
    s.set(f_to_c(66.4f));   // 0.4 F warm: inside the default 0.5 F cool differential
    HysteresisLaw law(0.1f);
    FakeOut h, c;
    OutputGuard guard(&h, &c, {});
    MemStore store;
    Controller ctl(port, s, law, guard, &store, Controller::Config{});
    ctl.begin(0);
    ctl.tick(0);
    CHECK(port.settings_n >= 1);                 // published at start
    CHECK_NEAR(c_to_f(port.settings.crash_default_c), 34.0, 0.01);
    CHECK_EQ(port.settings.cool_delay_s, static_cast<uint16_t>(180));
    CHECK(port.last.action == Action::Idle);   // the config's 0.5 F replaced the law's own 0.1 C

    port.in.push(Command::setTuning(Tuning::CoolDiff, 17));   // 0.17 C = 0.3 F
    ctl.tick(20);
    CHECK(port.last.action == Action::Cooling);
    CHECK_NEAR(port.settings.cool_diff_c, 0.17, 1e-4);
    CHECK_EQ(store.i["cdiff_cc"], 17);
    port.in.push(Command::setTuning(Tuning::CoolDelay, 600));
    port.in.push(Command::setTuning(Tuning::CrashDefault, 0));
    port.in.push(Command::setTuning(Tuning::HeatDiff, 900));   // 9 C: out of range, ignored
    ctl.tick(40);
    CHECK_EQ(port.settings.cool_delay_s, static_cast<uint16_t>(600));
    CHECK_NEAR(port.settings.heat_diff_c, 0.5f * 5.0f / 9.0f, 1e-4);

    /* Cold crash starts at its setting; resuming a paused crash keeps what was adjusted */
    port.in.push(Command::setMode(Mode::Crash, false));
    ctl.tick(60);
    CHECK_NEAR(port.last.setpoint_c[1], 0.0, 1e-4);
    port.in.push(Command::setSetpoint(Mode::Crash, 1.0f));
    port.in.push(Command::setMode(Mode::Crash, true));
    port.in.push(Command::setMode(Mode::Crash, false));
    ctl.tick(80);
    CHECK_NEAR(port.last.setpoint_c[1], 1.0, 1e-4);

    /* a restart brings them back */
    CapturePort port2;
    Controller ctl2(port2, s, law, guard, &store, Controller::Config{});
    ctl2.begin(0);
    ctl2.tick(0);
    CHECK_NEAR(port2.settings.cool_diff_c, 0.17, 1e-4);
    CHECK_EQ(port2.settings.cool_delay_s, static_cast<uint16_t>(600));
    CHECK_NEAR(port2.settings.crash_default_c, 0.0, 1e-4);

    /* re-sent every few seconds, so a display that missed them catches up */
    const int n0 = port2.settings_n;
    for(uint32_t t = 20; t <= 5020; t += 20) ctl2.tick(t);
    CHECK(port2.settings_n > n0);

    /* and forgetting them goes back to the config's */
    Controller::forgetTuning(store);
    CapturePort port3;
    Controller ctl3(port3, s, law, guard, &store, Controller::Config{});
    ctl3.begin(0);
    ctl3.tick(0);
    CHECK_EQ(port3.settings.cool_delay_s, static_cast<uint16_t>(180));
}

TEST(controller_runs_a_schedule_with_ramps_overrides_and_restarts)
{
    CapturePort port;
    FakeSensor s;
    s.set(19.0f);
    HysteresisLaw law(0.3f);
    FakeOut h, c;
    OutputGuard guard(&h, &c, {});
    MemStore store;
    Controller::Config cfg;
    cfg.clock_save_s = 60;
    cfg.setpoint_c[0] = 18.0f;
    uint32_t now = 0;
    Controller ctl(port, s, law, guard, &store, cfg);
    ctl.begin(now);
    ctl.tick(now);
    CHECK(port.settings.profile == Profile::Constant);
    CHECK_EQ(port.settings.nsteps, 0);                       // empty until the user builds one
    port.in.push(Command::setProfile(Profile::Schedule));     // nothing to run: refused
    ctl.tick(now += 20);
    CHECK(port.settings.profile == Profile::Constant);

    /* 18 C for 2 h (jump), then ramp to 22 C over 2 h and hold it 2 h */
    port.in.push(Command::setStep(0, 18.0f, 0, 2));
    port.in.push(Command::setStep(1, 22.0f, 2, 2));
    port.in.push(Command::setStepCount(2));
    port.in.push(Command::setStep(0, 18.0f, 0, 0));          // a hold of 0: refused
    port.in.push(Command::setProfile(Profile::Schedule));
    ctl.tick(now += 20);
    CHECK(port.settings.profile == Profile::Schedule);
    CHECK_EQ(port.settings.steps[0].hold_h, 2);
    CHECK_EQ(port.settings.step, 0);
    CHECK_NEAR(port.last.setpoint_c[0], 18.0, 1e-3);

    run_s(ctl, now, 1800);                                   // 0:30 into step 1: the dial overrides it
    port.in.push(Command::setSetpoint(Mode::Ferment, 19.0f));
    ctl.tick(now += 20);
    CHECK(port.settings.override_on);
    CHECK_NEAR(port.last.setpoint_c[0], 19.0, 1e-3);
    run_s(ctl, now, 3600);                                   // 1:30: still step 1, still overridden
    CHECK_NEAR(port.last.setpoint_c[0], 19.0, 1e-3);
    CHECK_EQ(store.i["sch_ovr_step"], 0);

    /* a power cut at 1:30 */
    CapturePort port2;
    uint32_t now2 = 0;
    Controller ctl2(port2, s, law, guard, &store, cfg);
    ctl2.begin(now2);
    ctl2.tick(now2);
    CHECK(port2.settings.profile == Profile::Schedule);
    CHECK(port2.settings.override_on);                        // the override survives too
    CHECK_NEAR(port2.last.setpoint_c[0], 19.0, 1e-3);

    run_s(ctl2, now2, 1800 + 3600);                           // ~3:00: halfway up the ramp, override gone
    CHECK_EQ(port2.settings.step, 1);
    CHECK(!port2.settings.override_on);
    CHECK(port2.last.setpoint_c[0] > 19.7f && port2.last.setpoint_c[0] < 20.3f);
    run_s(ctl2, now2, 3600 + 600);                            // ~4:10: holding 22
    CHECK_NEAR(port2.last.setpoint_c[0], 22.0, 1e-3);
    run_s(ctl2, now2, 2 * 3600);                              // past the end: done, still 22
    CHECK_EQ(port2.settings.step, Settings::kDone);
    CHECK_NEAR(port2.last.setpoint_c[0], 22.0, 1e-3);
    port2.in.push(Command::setSetpoint(Mode::Ferment, 21.0f));   /* finished: the dial still sets the target */
    ctl2.tick(now2 += 20);
    run_s(ctl2, now2, 3600);
    CHECK(port2.settings.override_on);
    CHECK_NEAR(port2.last.setpoint_c[0], 21.0, 1e-3);

    /* Cold crash ignores the schedule; stopping it keeps the target it reached */
    port2.in.push(Command::setMode(Mode::Crash, false));
    ctl2.tick(now2 += 20);
    CHECK(port2.last.mode == Mode::Crash);
    port2.in.push(Command::setMode(Mode::Ferment, false));
    port2.in.push(Command::setProfile(Profile::Constant));
    ctl2.tick(now2 += 20);
    CHECK(port2.settings.profile == Profile::Constant);
    CHECK_NEAR(port2.last.setpoint_c[0], 21.0, 1e-3);
    CHECK_EQ(store.i["sp0_cc"], 2100);

    /* removing every step of a running schedule goes back to Constant */
    port2.in.push(Command::setProfile(Profile::Schedule));
    port2.in.push(Command::setStepCount(0));
    ctl2.tick(now2 += 20);
    CHECK_EQ(port2.settings.nsteps, 0);
    CHECK(port2.settings.profile == Profile::Constant);
}

TEST(controller_publishes_what_it_wants_while_the_guard_holds_it_off)
{
    CapturePort port;
    FakeSensor s;
    s.set(f_to_c(68.6f));
    DeadbandLaw law(0.3f * 5.0f / 9.0f);
    FakeOut h, c;
    OutputGuard guard(&h, &c, {});
    Controller::Config cfg;
    cfg.cool_delay_s = 300;   // 5 min cooling restart delay (the controller sets the guard's delays)
    Controller ctl(port, s, law, guard, nullptr, cfg);
    ctl.begin(0);
    ctl.tick(0);
    CHECK(port.last.action == Action::Cooling && c.on);   // nothing held off at power-up
    s.set(f_to_c(66.0f));
    ctl.tick(1500);   // evaluates every 1.5 s
    CHECK(port.last.action == Action::Idle);              // cooling stopped at 1500
    s.set(f_to_c(68.6f));
    ctl.tick(3000);
    CHECK(port.last.action == Action::Idle);      // held off...
    CHECK(port.last.wanted == Action::Cooling);   // ...but it wants to cool
    CHECK_EQ(port.last.wait_s, static_cast<uint16_t>(299));   // 298.5 s, rounded up
    ctl.tick(61500);
    CHECK_EQ(port.last.wait_s, static_cast<uint16_t>(240));
    ctl.tick(301500);
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
    cfg.history_period_ms = 60000;
    Controller ctl(port, s, law, guard, nullptr, cfg);
    float h[50];
    for(int i = 0; i < 50; i++) h[i] = 10.0f + i;
    ctl.history().assign(h, 50);
    ctl.begin(0);
    CHECK_EQ(port.hist.size(), static_cast<size_t>(1));   // begin() starts a sync: one chunk now...
    CHECK(port.hist[0].reset);
    CHECK(ctl.syncing());
    ctl.tick(10);
    ctl.tick(20);
    CHECK_EQ(port.hist.size(), static_cast<size_t>(3));   // ...and one per tick after that
    CHECK(!ctl.syncing());
    port.hist.clear();

    port.in.push(Command::requestSync());
    for(uint32_t t = 30; t <= 60; t += 10) ctl.tick(t);
    CHECK_EQ(port.hist.size(), static_cast<size_t>(3));   // 24 + 24 + 2
    CHECK(port.hist[0].reset);
    CHECK_EQ(port.hist[0].ms_per_sample, 60000u);
    CHECK(!port.hist[1].reset);
    CHECK(port.hist[0].more && port.hist[1].more && !port.hist[2].more);
    CHECK_EQ(port.hist[2].n, static_cast<uint8_t>(2));
    CHECK_NEAR(port.hist[2].temps_c[1], 59.0, 1e-6);

    port.hist.clear();
    ctl.tick(60000);   // one sample per period
    CHECK_EQ(port.hist.size(), static_cast<size_t>(1));
    CHECK_EQ(port.hist[0].n, static_cast<uint8_t>(1));
    CHECK_NEAR(port.hist[0].temps_c[0], 18.0, 1e-6);
    CHECK_EQ(ctl.history().size(), 51);
}

TEST(controller_full_history_sync_waits_for_room_and_keeps_order)
{
    CapturePort port;
    port.room = 2;                                 // a small queue the "UI" drains slowly
    FakeSensor s;
    s.set(18.0f);
    DeadbandLaw law(0.1f);
    OutputGuard guard(nullptr, nullptr, {});
    Controller ctl(port, s, law, guard, nullptr, Controller::Config{});   // 200 ms samples
    static float h[History::kCapacity];
    for(int i = 0; i < History::kCapacity; i++) h[i] = static_cast<float>(i);
    ctl.history().assign(h, History::kCapacity);   // a full 1 h window
    ctl.begin(0);
    std::vector<float> got;
    bool reset_first = false;
    uint32_t t = 0;
    for(int guard_n = 0; guard_n < 10000 && (ctl.syncing() || port.hist.size()); guard_n++) {
        t += 20;
        ctl.tick(t);                               // a new sample every 200 ms lands mid-sync
        if(guard_n % 3 == 0 && !port.hist.empty()) {   // the UI takes one chunk now and then
            const HistoryChunk c = port.hist.front();
            port.hist.erase(port.hist.begin());
            if(c.reset) { got.clear(); reset_first = true; }
            for(int i = 0; i < c.n; i++) {   // like the UI: the oldest drops off when full
                got.push_back(c.temps_c[i]);
                if(got.size() > static_cast<size_t>(History::kCapacity)) got.erase(got.begin());
            }
        }
    }
    CHECK(reset_first);
    CHECK(!ctl.syncing());
    CHECK_EQ(got.size(), static_cast<size_t>(ctl.history().size()));
    bool same = got.size() == static_cast<size_t>(ctl.history().size());
    for(size_t i = 0; same && i < got.size(); i++) same = got[i] == ctl.history().at(static_cast<int>(i));
    CHECK(same);                                   // nothing lost, nothing out of order
    CHECK_NEAR(ctl.history().at(ctl.history().size() - 1), 18.0, 1e-6);   // includes new samples
}

TEST(controller_history_sample_is_the_mean_reading)
{
    CapturePort port;
    FakeSensor s;
    DeadbandLaw law(0.1f);
    OutputGuard guard(nullptr, nullptr, {});
    Controller::Config cfg;
    cfg.history_period_ms = 60000;
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
