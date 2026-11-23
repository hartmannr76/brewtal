/**
 * Demo / simulator stand-in for a fermenter: a TempSensor whose temperature follows a simple
 * heat balance. Portable; used by the desktop sim, the web harness and boards built with
 * BREW_DEMO=1.
 *
 *   C * dT/dt = P_heat - P_cool + P_yeast + UA * (T_ambient - T)
 *
 *   C        heat capacity of the beer (volume x ~4.18 kJ/(L*K), it is mostly water)
 *   P_heat   heater power: heat wrap / belt / pad (W)
 *   P_cool   cooling power into the beer: glycol coil / chiller / fridge (W)
 *   P_yeast  heat from active fermentation (W)
 *   UA       heat loss to the room per degree (W/K): insulation. Bare stainless ~3,
 *            neoprene jacket ~1.8, thick foam jacket ~0.8, inside a chest freezer ~0.5
 *
 * Elements don't switch instantly: their delivered power follows the relay with a first-order
 * lag (heat_lag_s / cool_lag_s; 0 by default = instant). A heat wrap keeps heating after it turns
 * off, which is where overshoot comes from.
 *
 * Wire it between the controller and the real relays:
 *     FakePlant plant(cfg);
 *     OutputGuard guard(&plant.heat(&real_heat), &plant.cool(&real_cool), ...);
 */
#pragma once

#include "brew/hal/io.h"
#include "brew/hal/sensor.h"

#include <cmath>
#include <cstdint>

namespace brew {

class FakePlant final : public hal::TempSensor {
public:
    /** Defaults: 5 gal in a neoprene jacket, 50 W heat wrap, 150 W glycol coil, 68 F room. */
    struct Config {
        float start_c = 20.2f;          // 68.4 F
        float ambient_c = 20.0f;        // room (or chamber) temperature
        float volume_l = 18.9f;         // 5 US gal
        float heat_w = 50.0f;           // heat wrap
        float cool_w = 150.0f;          // glycol coil with a small chiller
        float loss_w_per_k = 1.8f;      // UA: neoprene jacket on a 5 gal fermenter
        float heat_lag_s = 0.0f;        // e.g. 240: a wrap has mass, warms up and cools down slowly
        float cool_lag_s = 0.0f;        // e.g. 60: glycol in the coil keeps cooling briefly
        float yeast_w = 0.0f;           // active ale fermentation, 5 gal: ~10-20 W at peak
        float quantum_c = 0.0625f;      // DS18B20 resolution; 0 = exact
        bool  frozen = false;           // never changes on its own (scripted sims)

        float capacity_j_per_k() const { return volume_l * 4180.0f; }
    };

    class Tap final : public hal::DigitalOut {
    public:
        void write(bool v) override
        {
            on = v;
            if(fwd) fwd->write(v);
        }
        bool on = false;
        hal::DigitalOut * fwd = nullptr;
    };

    explicit FakePlant(const Config & cfg) : cfg_(cfg), t_(cfg.start_c) {}

    hal::DigitalOut & heat(hal::DigitalOut * forward = nullptr) { heat_.fwd = forward; return heat_; }
    hal::DigitalOut & cool(hal::DigitalOut * forward = nullptr) { cool_.fwd = forward; return cool_; }

    /** Scripted jump (simulator). */
    void set(float c) { t_ = c; }
    float actual() const { return t_; }
    const Config & config() const { return cfg_; }
    /** Power the elements are delivering right now (W), after their lag. */
    float heatPowerW() const { return p_heat_; }
    float coolPowerW() const { return p_cool_; }

    /** Swap the physical setup, keeping the beer temperature and element state. */
    void reconfigure(const Config & cfg) { cfg_ = cfg; }

    bool begin() override { return true; }

    void update(uint32_t now) override
    {
        if(!started_) {
            started_ = true;
            last_ = now;
        }
        float dt = static_cast<float>(now - last_) / 1000.0f;
        last_ = now;
        now_ = now;
        if(cfg_.frozen) return;
        const float cap = cfg_.capacity_j_per_k() > 1.0f ? cfg_.capacity_j_per_k() : 1.0f;
        /* sub-steps keep the integration stable for long gaps (sim speed-up, slow callers) */
        while(dt > 0.0f) {
            const float h = dt > 5.0f ? 5.0f : dt;
            dt -= h;
            p_heat_ = lag(p_heat_, heat_.on ? cfg_.heat_w : 0.0f, cfg_.heat_lag_s, h);
            p_cool_ = lag(p_cool_, cool_.on ? cfg_.cool_w : 0.0f, cfg_.cool_lag_s, h);
            const float q = p_heat_ - p_cool_ + cfg_.yeast_w + cfg_.loss_w_per_k * (cfg_.ambient_c - t_);
            t_ += q / cap * h;
        }
    }

    hal::TempReading latest() const override
    {
        hal::TempReading r;
        r.celsius = cfg_.quantum_c > 0 ? std::round(t_ / cfg_.quantum_c) * cfg_.quantum_c : t_;
        r.valid = true;
        r.at_ms = now_;
        return r;
    }

    const char * name() const override { return "fake-plant"; }

private:
    static float lag(float p, float target, float tau_s, float dt)
    {
        if(tau_s <= 0.0f) return target;
        return p + (target - p) * (1.0f - std::exp(-dt / tau_s));
    }

    Config cfg_;
    float t_;
    float p_heat_ = 0.0f, p_cool_ = 0.0f;
    Tap heat_, cool_;
    bool started_ = false;
    uint32_t last_ = 0, now_ = 0;
};

/**
 * Plausible-looking past for demos and the simulator: `n` samples settling from
 * target + swing toward `end_f`, with some wobble. Fahrenheit inputs (matches the mockups).
 */
template<typename HistoryT>
void seedDemoHistory(HistoryT & h, int n, float end_f, float target_f, float swing_f)
{
    float c[96];
    if(n > 96) n = 96;
    for(int i = 0; i < n; i++) {
        const float p = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 1.0f;
        const float f = target_f + (1 - p) * swing_f + std::sin(i * 0.42f + 1.3f) * 0.55f * (1 - p * 0.7f) +
                        std::sin(i * 1.1f) * 0.18f * (1 - p) + (end_f - target_f) * p * p;
        c[i] = (f - 32.0f) * 5.0f / 9.0f;
    }
    if(n > 0) c[n - 1] = (end_f - 32.0f) * 5.0f / 9.0f;
    h.assign(c, n);
}

}  // namespace brew
