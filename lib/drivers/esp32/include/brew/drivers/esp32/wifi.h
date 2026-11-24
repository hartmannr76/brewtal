/**
 * The ESP32's WiFi radio (esp_wifi) as a hal::Wifi: a station for the user's network and,
 * while setup runs, a WPA2 access point (192.168.4.1) for the phone. ESP-IDF APIs only.
 *
 * Events arrive on the ESP-IDF event task and are kept in atomics; the hal::Wifi calls are made
 * from one task (the UI task). Credentials live in RAM only (WIFI_STORAGE_RAM): saving them is
 * WifiSetup's job, through SettingsStore.
 */
#pragma once

#include "brew/drivers/interfaces/wifi.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace brew::drivers {

class EspWifi final : public hal::Wifi {
public:
    struct Config {
        uint8_t ap_channel = 1;
        uint8_t ap_max_clients = 2;
    };

    EspWifi() = default;
    explicit EspWifi(const Config & cfg) : cfg_(cfg) {}

    bool begin() override;
    uint32_t deviceId() override;
    uint32_t random() override;

    bool startAccessPoint(const char * ssid, const char * password) override;
    void stopAccessPoint() override;
    int accessPointClients() override { return clients_.load(); }

    bool startScan() override;
    int scanResults(hal::WifiNetwork * out, int max) override;

    bool connect(const char * ssid, const char * password) override;
    void disconnect() override;
    hal::WifiLink link() override { return static_cast<hal::WifiLink>(link_.load()); }

    /* called from the ESP-IDF event task */
    void onEvent(const char * base, int32_t id, void * data);

private:
    Config cfg_;
    bool started_ = false;
    void * ap_netif_ = nullptr;   // esp_netif_t of the setup network
    bool ap_on_ = false;
    std::atomic<int> clients_{ 0 };
    std::atomic<bool> scanning_{ false };
    std::atomic<uint8_t> link_{ static_cast<uint8_t>(hal::WifiLink::Down) };
};

}  // namespace brew::drivers
