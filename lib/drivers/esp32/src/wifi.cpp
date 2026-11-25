#include "brew/drivers/esp32/wifi.h"
#include "brew/drivers/esp32/log.h"

#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"

#include <cstdio>
#include <cstring>

namespace brew::drivers {

namespace {
const char * TAG = "brew.wifi";

void on_event(void * arg, esp_event_base_t base, int32_t id, void * data)
{
    static_cast<EspWifi *>(arg)->onEvent(base, id, data);
}

/* Why a connect attempt ended, in the terms WifiSetup shows the user. */
hal::WifiLink why(uint8_t reason)
{
    switch(reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return hal::WifiLink::WrongPassword;
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return hal::WifiLink::NotFound;
        default:
            return hal::WifiLink::Failed;
    }
}

/* Phones decide whether a network has a sign-in page in two ways, and the setup network
 * serves both:
 *  - DHCP option 114 (RFC 8910, "captive portal URI"): iOS 14+ / Android 11+ open the page
 *    named here as soon as they join.
 *  - Older phones load a known test page; for that the phone must use our DNS (captive_dns),
 *    so the DHCP offer names 192.168.4.1 as its DNS server explicitly. */
void setup_ap_dhcp(esp_netif_t * ap)
{
    esp_netif_ip_info_t ip = {};
    esp_netif_get_ip_info(ap, &ip);
    char uri[40];
    std::snprintf(uri, sizeof(uri), "http://" IPSTR "/", IP2STR(&ip.ip));

    /* As ESP-IDF's captive portal example: once the access point is up, stop the DHCP server,
     * set the options, start it again. Every step is logged, since a phone that gets no
     * address can reach nothing at all. */
    esp_err_t e = esp_netif_dhcps_stop(ap);
    if(e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) BREW_LOGW(TAG, "DHCP stop: %s", esp_err_to_name(e));
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    e = esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    if(e != ESP_OK) BREW_LOGW(TAG, "DHCP DNS address: %s", esp_err_to_name(e));
    dhcps_offer_t offer = OFFER_DNS;
    e = esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    if(e != ESP_OK) BREW_LOGW(TAG, "DHCP offer DNS: %s", esp_err_to_name(e));
    e = esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, std::strlen(uri));
    if(e != ESP_OK) BREW_LOGW(TAG, "DHCP captive portal URI: %s", esp_err_to_name(e));
    e = esp_netif_dhcps_start(ap);
    if(e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) BREW_LOGE(TAG, "DHCP start: %s", esp_err_to_name(e));
    BREW_LOGI(TAG, "setup network at " IPSTR ": DHCP %s, DNS " IPSTR ", sign-in page %s", IP2STR(&ip.ip),
             e == ESP_OK || e == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ? "running" : "NOT running", IP2STR(&ip.ip), uri);
}

void copy(uint8_t * dst, size_t n, const char * src)
{
    std::memset(dst, 0, n);
    if(src) std::memcpy(dst, src, std::strlen(src) < n ? std::strlen(src) : n);
}
}  // namespace

bool EspWifi::begin()
{
    if(started_) return true;
    esp_netif_init();
    const esp_err_t loop = esp_event_loop_create_default();
    if(loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {   // INVALID_STATE: someone made it already
        BREW_LOGE(TAG, "event loop: %s", esp_err_to_name(loop));
        return false;
    }
    esp_netif_create_default_wifi_sta();
    ap_netif_ = esp_netif_create_default_wifi_ap();   // 192.168.4.1, with a DHCP server
    if(!ap_netif_) BREW_LOGE(TAG, "no access point interface");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if(esp_wifi_init(&init) != ESP_OK) {
        BREW_LOGE(TAG, "esp_wifi_init failed");
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, this, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, this, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, this, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_event, this, nullptr);
    esp_wifi_set_mode(WIFI_MODE_STA);
    const esp_err_t st = esp_wifi_start();
    if(st != ESP_OK) {
        BREW_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(st));
        return false;
    }
    started_ = true;
    BREW_LOGI(TAG, "radio started");
    return true;
}

uint32_t EspWifi::deviceId()
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    return (static_cast<uint32_t>(mac[3]) << 16) | (static_cast<uint32_t>(mac[4]) << 8) | mac[5];
}

uint32_t EspWifi::random() { return esp_random(); }

bool EspWifi::startAccessPoint(const char * ssid, const char * password)
{
    wifi_config_t c = {};
    copy(c.ap.ssid, sizeof(c.ap.ssid), ssid);
    c.ap.ssid_len = static_cast<uint8_t>(std::strlen(ssid) > 32 ? 32 : std::strlen(ssid));
    copy(c.ap.password, sizeof(c.ap.password), password);
    c.ap.channel = cfg_.ap_channel;
    c.ap.max_connection = cfg_.ap_max_clients;
    c.ap.authmode = password && std::strlen(password) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.ap.pmf_cfg.required = false;
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if(e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &c);
    if(e != ESP_OK) {
        BREW_LOGE(TAG, "access point %s: %s", ssid, esp_err_to_name(e));
        return false;
    }
    ap_on_ = true;
    clients_ = 0;
    BREW_LOGI(TAG, "setup network %s up", ssid);
    return true;
}

void EspWifi::stopAccessPoint()
{
    if(!ap_on_) return;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ap_on_ = false;
    clients_ = 0;
    BREW_LOGI(TAG, "setup network down");
}

bool EspWifi::startScan()
{
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    if(esp_wifi_scan_start(&sc, false) != ESP_OK) return false;   // e.g. busy connecting
    scanning_ = true;
    return true;
}

int EspWifi::scanResults(hal::WifiNetwork * out, int max)
{
    if(scanning_) return -1;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if(n == 0) return 0;
    if(n > 32) n = 32;
    wifi_ap_record_t * recs = new wifi_ap_record_t[n];
    if(esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) n = 0;
    int k = 0;
    for(uint16_t i = 0; i < n && k < max; i++, k++) {
        std::snprintf(out[k].ssid, sizeof(out[k].ssid), "%s", reinterpret_cast<const char *>(recs[i].ssid));
        out[k].rssi = recs[i].rssi;
        out[k].secured = recs[i].authmode != WIFI_AUTH_OPEN;
    }
    delete[] recs;
    return k;
}

bool EspWifi::connect(const char * ssid, const char * password)
{
    wifi_config_t c = {};
    copy(c.sta.ssid, sizeof(c.sta.ssid), ssid);
    copy(c.sta.password, sizeof(c.sta.password), password);
    c.sta.threshold.authmode = password && password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;   // WPA3 routers too
    c.sta.pmf_cfg.capable = true;
    esp_wifi_disconnect();
    if(esp_wifi_set_config(WIFI_IF_STA, &c) != ESP_OK) {
        link_ = static_cast<uint8_t>(hal::WifiLink::Failed);
        return false;
    }
    link_ = static_cast<uint8_t>(hal::WifiLink::Connecting);
    if(esp_wifi_connect() != ESP_OK) {
        link_ = static_cast<uint8_t>(hal::WifiLink::Failed);
        return false;
    }
    BREW_LOGI(TAG, "joining %s", ssid);
    return true;
}

void EspWifi::disconnect()
{
    link_ = static_cast<uint8_t>(hal::WifiLink::Down);   // first, so the event below reads as asked for
    esp_wifi_disconnect();
}

void EspWifi::onEvent(const char * base, int32_t id, void * data)
{
    if(base == WIFI_EVENT) {
        switch(id) {
            case WIFI_EVENT_STA_DISCONNECTED: {
                const uint8_t reason = static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
                const auto l = static_cast<hal::WifiLink>(link_.load());
                if(l == hal::WifiLink::Connecting) {
                    link_ = static_cast<uint8_t>(why(reason));
                    BREW_LOGW(TAG, "join failed (reason %u)", reason);
                }
                else if(l == hal::WifiLink::Up) {
                    link_ = static_cast<uint8_t>(hal::WifiLink::Down);
                    BREW_LOGW(TAG, "link lost (reason %u)", reason);
                }
                break;
            }
            case WIFI_EVENT_AP_STACONNECTED: {
                clients_++;
                const uint8_t * m = static_cast<wifi_event_ap_staconnected_t *>(data)->mac;
                BREW_LOGI(TAG, "phone joined the setup network (" MACSTR ")", MAC2STR(m));
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED:
                if(clients_ > 0) clients_--;
                break;
            case WIFI_EVENT_SCAN_DONE: scanning_ = false; break;
            case WIFI_EVENT_AP_START:   // the setup network is up: now its DHCP options can be set
                if(ap_netif_) setup_ap_dhcp(static_cast<esp_netif_t *>(ap_netif_));
                break;
            default: break;
        }
    }
    else if(base == IP_EVENT) {
        if(id == IP_EVENT_STA_GOT_IP) {
            link_ = static_cast<uint8_t>(hal::WifiLink::Up);
            const auto * e = static_cast<ip_event_got_ip_t *>(data);
            ip_ = (static_cast<uint32_t>(esp_ip4_addr1(&e->ip_info.ip)) << 24) |
                  (static_cast<uint32_t>(esp_ip4_addr2(&e->ip_info.ip)) << 16) |
                  (static_cast<uint32_t>(esp_ip4_addr3(&e->ip_info.ip)) << 8) | esp_ip4_addr4(&e->ip_info.ip);
            BREW_LOGI(TAG, "online, address " IPSTR, IP2STR(&e->ip_info.ip));
        }
        else if(id == IP_EVENT_STA_LOST_IP) {
            link_ = static_cast<uint8_t>(hal::WifiLink::Down);
            ip_ = 0;
        }
        else if(id == IP_EVENT_AP_STAIPASSIGNED) {
            const auto * e = static_cast<ip_event_ap_staipassigned_t *>(data);
            BREW_LOGI(TAG, "gave the phone address " IPSTR, IP2STR(&e->ip));
        }
    }
}

}  // namespace brew::drivers
