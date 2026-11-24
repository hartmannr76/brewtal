/**
 * WiFi radio: join a network as a station, and run a small access point of its own (the setup
 * network a phone joins to reach the setup page). Every call returns at once; progress is read
 * back with link() / scanResults() / accessPointClients(), which the caller polls.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace brew::hal {

struct WifiNetwork {
    char   ssid[33] = { 0 };   // up to 32 bytes + terminator
    int8_t rssi = 0;           // dBm, e.g. -48 strong .. -90 weak
    bool   secured = false;    // needs a password
};

/** The station side: our link to the user's network. */
enum class WifiLink : uint8_t {
    Down = 0,        // not connected, not trying
    Connecting,
    Up,              // associated and has an address
    WrongPassword,   // the last connect() was refused for its password
    NotFound,        // the last connect() found no such network
    Failed,          // the last connect() failed for another reason
};

class Wifi {
public:
    virtual ~Wifi() = default;
    virtual bool begin() = 0;

    /** A number unique to this device (the low bits of its MAC), for its setup network's name. */
    virtual uint32_t deviceId() = 0;
    /** A random number from a real entropy source (for the setup network's password). */
    virtual uint32_t random() = 0;

    /* access point (the setup network) */
    virtual bool startAccessPoint(const char * ssid, const char * password) = 0;
    virtual void stopAccessPoint() = 0;
    virtual int  accessPointClients() = 0;   // phones joined to it

    /* scanning */
    virtual bool startScan() = 0;
    /** Copies up to `max` networks found by the last scan; -1 while a scan is running. */
    virtual int  scanResults(WifiNetwork * out, int max) = 0;

    /* station */
    virtual bool connect(const char * ssid, const char * password) = 0;   // starts; see link()
    virtual void disconnect() = 0;
    virtual WifiLink link() = 0;
};

}  // namespace brew::hal
