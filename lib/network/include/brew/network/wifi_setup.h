/**
 * WiFi setup and keeping the device online. Portable: talks only to hal::Wifi and
 * hal::SettingsStore, so it is unit-tested on the host and runs unchanged in the web harness.
 *
 *   first start, nothing saved  ->  Hotspot: the setup network is up, its name and password
 *                                   are on the dial (and in a QR code)
 *   a phone joins it            ->  PhoneJoined: the setup page is open on the phone
 *   the user picks a network    ->  Connecting (the setup network stays up meanwhile)
 *     refused                   ->  back to Hotspot/PhoneJoined with an error on dial and page
 *     joined                    ->  Connected: saved; the setup network lingers a few seconds
 *                                   so the page can say so, then turns off
 *   later, the link drops       ->  Offline: retries in the background, never reopens setup
 *
 * Setup also ends when the user skips it (a press on the dial) or after setup_timeout_ms.
 * Temperature control never waits on any of this.
 */
#pragma once

#include "brew/drivers/interfaces/platform.h"
#include "brew/drivers/interfaces/wifi.h"

#include <cstddef>
#include <cstdint>

namespace brew::net {

enum class SetupPhase : uint8_t {
    Off = 0,       // no WiFi saved and setup isn't running (skipped or timed out)
    Hotspot,       // setup network up, waiting for a phone
    PhoneJoined,   // a phone is on the setup network
    Connecting,    // trying the network the user picked
    Connected,     // on the user's WiFi
    Offline,       // WiFi saved but not connected: retrying in the background
};

enum class SetupError : uint8_t { None = 0, WrongPassword, NotFound, Failed };

const char * phaseName(SetupPhase p);   // "hotspot", "phone_joined", ...
const char * errorName(SetupError e);   // "", "wrong_password", ...

struct SetupStatus {
    SetupPhase phase = SetupPhase::Off;
    SetupError error = SetupError::None;   // why the last attempt failed (setup only)
    char ssid[33] = { 0 };                 // the network joined, being tried, or saved
    char ap_ssid[33] = { 0 };              // the setup network: "BrewDial-4F2A"
    char ap_password[20] = { 0 };          // ...and its password: "malt-7391"
    int  clients = 0;                      // phones on the setup network
    bool scanning = false;
    uint32_t setup_left_ms = 0;            // until setup closes by itself
};

class WifiSetup {
public:
    struct Config {
        uint32_t setup_timeout_ms = 15u * 60u * 1000u;   // the setup network closes after this
        uint32_t linger_ms = 10000;                      // keeps it up after success, for the page
        uint32_t connect_timeout_ms = 20000;             // one attempt from the setup page
        uint32_t retry_min_ms = 5000;                    // background reconnect back-off...
        uint32_t retry_max_ms = 5u * 60u * 1000u;        // ...doubling up to this
    };
    static constexpr int kMaxNetworks = 20;

    WifiSetup(hal::Wifi & wifi, hal::SettingsStore * store, const Config & cfg);

    /** Saved WiFi: joins it quietly. Nothing saved: opens setup. */
    void begin(uint32_t now_ms);
    void tick(uint32_t now_ms);

    /** Opens setup (the WiFi menu item). Keeps any current connection until a new one is picked. */
    void startSetup(uint32_t now_ms);
    /** Closes setup without changing anything (a press on the dial, or "Set up later"). */
    void cancelSetup(uint32_t now_ms);
    /** From the setup page: try this network. Saved only once it works. */
    bool submit(const char * ssid, const char * password, uint32_t now_ms);
    void rescan();
    /** Forgets the saved network (factory reset); setup opens again. */
    void forget(uint32_t now_ms);

    bool setupOpen() const;
    const SetupStatus & status() const { return st_; }
    int networks(const hal::WifiNetwork *& out) const { out = nets_; return nnets_; }

    /** "malt-7391": a brewing word and four digits (WPA2 needs at least 8 characters). */
    static void makePassword(uint32_t rnd, char * out, size_t n);

private:
    void setPhase(SetupPhase p);
    void openAp(uint32_t now);
    void closeAp();
    void attempt(const char * ssid, const char * password, uint32_t now);
    void afterSetup(uint32_t now);   // the phase to fall back to when setup closes
    void pullScan();

    hal::Wifi & wifi_;
    hal::SettingsStore * store_;
    Config cfg_;
    SetupStatus st_;

    bool ap_on_ = false;
    uint32_t setup_until_ = 0;     // setup closes at this time
    uint32_t linger_until_ = 0;    // after success: the setup network turns off at this time
    bool lingering_ = false;
    uint32_t attempt_until_ = 0;   // the current setup attempt gives up at this time

    char saved_ssid_[33] = { 0 };
    char saved_pw_[65] = { 0 };
    char try_ssid_[33] = { 0 };
    char try_pw_[65] = { 0 };

    uint32_t retry_at_ = 0;
    uint32_t retry_ms_ = 0;
    bool trying_saved_ = false;

    hal::WifiNetwork nets_[kMaxNetworks];
    int nnets_ = 0;
};

}  // namespace brew::net
