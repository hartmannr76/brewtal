#include "brew/network/wifi_setup.h"

#include <cstdio>
#include <cstring>

namespace brew::net {

namespace {
/* Settings keys (typed key/value pairs; NVS keys are at most 15 characters). */
constexpr const char * kKeySsid = "wifi_ssid";
constexpr const char * kKeyPass = "wifi_pass";

/* Has `t` been reached? Unsigned millis, so this stays right across the 49-day wrap. */
bool reached(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t) >= 0; }

void copy(char * d, size_t n, const char * s) { std::snprintf(d, n, "%s", s ? s : ""); }

SetupError errorFor(hal::WifiLink l)
{
    switch(l) {
        case hal::WifiLink::WrongPassword: return SetupError::WrongPassword;
        case hal::WifiLink::NotFound:      return SetupError::NotFound;
        default:                           return SetupError::Failed;
    }
}
}  // namespace

const char * phaseName(SetupPhase p)
{
    switch(p) {
        case SetupPhase::Off:         return "off";
        case SetupPhase::Hotspot:     return "hotspot";
        case SetupPhase::PhoneJoined: return "phone_joined";
        case SetupPhase::Connecting:  return "connecting";
        case SetupPhase::Connected:   return "connected";
        case SetupPhase::Offline:     return "offline";
    }
    return "off";
}

const char * errorName(SetupError e)
{
    switch(e) {
        case SetupError::None:          return "";
        case SetupError::WrongPassword: return "wrong_password";
        case SetupError::NotFound:      return "not_found";
        case SetupError::Failed:        return "failed";
    }
    return "";
}

WifiSetup::WifiSetup(hal::Wifi & wifi, hal::SettingsStore * store, const Config & cfg)
    : wifi_(wifi), store_(store), cfg_(cfg)
{
}

void WifiSetup::makePassword(uint32_t rnd, char * out, size_t n)
{
    static const char * const kWords[] = { "malt", "hops", "wort", "mash", "yeast", "lager",
                                           "stout", "grist", "barley", "porter", "kettle", "krausen" };
    constexpr uint32_t kN = sizeof(kWords) / sizeof(kWords[0]);
    std::snprintf(out, n, "%s-%04u", kWords[rnd % kN], static_cast<unsigned>((rnd / kN) % 10000u));
}

void WifiSetup::begin(uint32_t now)
{
    wifi_.begin();
    std::snprintf(st_.ap_ssid, sizeof(st_.ap_ssid), "BrewDial-%04X", static_cast<unsigned>(wifi_.deviceId() & 0xFFFFu));
    if(store_) {
        if(!store_->getStr(kKeySsid, saved_ssid_, sizeof(saved_ssid_))) saved_ssid_[0] = '\0';
        if(!store_->getStr(kKeyPass, saved_pw_, sizeof(saved_pw_))) saved_pw_[0] = '\0';
    }
    if(saved_ssid_[0]) {
        copy(st_.ssid, sizeof(st_.ssid), saved_ssid_);
        setPhase(SetupPhase::Offline);
        retry_ms_ = cfg_.retry_min_ms;
        retry_at_ = now + retry_ms_;
        wifi_.connect(saved_ssid_, saved_pw_);
    }
    else {
        startSetup(now);
    }
}

bool WifiSetup::setupOpen() const
{
    return st_.phase == SetupPhase::Hotspot || st_.phase == SetupPhase::PhoneJoined ||
           st_.phase == SetupPhase::Connecting;
}

void WifiSetup::setPhase(SetupPhase p) { st_.phase = p; }

void WifiSetup::openAp(uint32_t now)
{
    makePassword(wifi_.random(), st_.ap_password, sizeof(st_.ap_password));
    wifi_.startAccessPoint(st_.ap_ssid, st_.ap_password);
    ap_on_ = true;
    lingering_ = false;
    setup_until_ = now + cfg_.setup_timeout_ms;
    st_.clients = 0;
    rescan();
}

void WifiSetup::closeAp()
{
    if(ap_on_) wifi_.stopAccessPoint();
    ap_on_ = false;
    lingering_ = false;
    st_.clients = 0;
    st_.scanning = false;
}

void WifiSetup::startSetup(uint32_t now)
{
    if(setupOpen()) {   // already open: just give it the full time again
        setup_until_ = now + cfg_.setup_timeout_ms;
        return;
    }
    st_.error = SetupError::None;
    openAp(now);
    setPhase(SetupPhase::Hotspot);
}

void WifiSetup::cancelSetup(uint32_t now)
{
    if(!setupOpen() && !lingering_) return;
    if(st_.phase == SetupPhase::Connecting) wifi_.disconnect();   // an unsaved attempt
    closeAp();
    st_.error = SetupError::None;
    afterSetup(now);
}

void WifiSetup::afterSetup(uint32_t now)
{
    copy(st_.ssid, sizeof(st_.ssid), saved_ssid_);
    if(!saved_ssid_[0]) {
        setPhase(SetupPhase::Off);
        return;
    }
    if(wifi_.link() == hal::WifiLink::Up) {
        setPhase(SetupPhase::Connected);
        return;
    }
    setPhase(SetupPhase::Offline);   // back to the saved network, right away
    retry_ms_ = cfg_.retry_min_ms;
    retry_at_ = now;
}

bool WifiSetup::submit(const char * ssid, const char * password, uint32_t now)
{
    if(!setupOpen() || !ssid || !ssid[0] || std::strlen(ssid) > 32) return false;
    if(password && std::strlen(password) > 64) return false;
    attempt(ssid, password ? password : "", now);
    return true;
}

void WifiSetup::attempt(const char * ssid, const char * password, uint32_t now)
{
    copy(try_ssid_, sizeof(try_ssid_), ssid);
    copy(try_pw_, sizeof(try_pw_), password);
    copy(st_.ssid, sizeof(st_.ssid), try_ssid_);
    st_.error = SetupError::None;
    wifi_.disconnect();
    wifi_.connect(try_ssid_, try_pw_);
    attempt_until_ = now + cfg_.connect_timeout_ms;
    setup_until_ = now + cfg_.setup_timeout_ms;   // someone is using it: keep it open
    setPhase(SetupPhase::Connecting);
}

void WifiSetup::rescan()
{
    if(wifi_.startScan()) st_.scanning = true;
}

void WifiSetup::forget(uint32_t now)
{
    saved_ssid_[0] = '\0';
    saved_pw_[0] = '\0';
    if(store_) {
        store_->setStr(kKeySsid, "");
        store_->setStr(kKeyPass, "");
        store_->commit();
    }
    wifi_.disconnect();
    closeAp();
    st_.ssid[0] = '\0';
    setPhase(SetupPhase::Off);
    startSetup(now);
}

void WifiSetup::pullScan()
{
    if(!st_.scanning) return;
    hal::WifiNetwork found[kMaxNetworks * 2];
    const int n = wifi_.scanResults(found, kMaxNetworks * 2);
    if(n < 0) return;   // still scanning
    st_.scanning = false;
    /* one entry per name (the strongest), strongest first, no hidden networks */
    nnets_ = 0;
    for(int i = 0; i < n; i++) {
        const hal::WifiNetwork & f = found[i];
        if(!f.ssid[0]) continue;
        int k = 0;
        while(k < nnets_ && std::strcmp(nets_[k].ssid, f.ssid) != 0) k++;
        if(k < nnets_) {
            if(f.rssi > nets_[k].rssi) nets_[k] = f;
        }
        else if(nnets_ < kMaxNetworks) {
            nets_[nnets_++] = f;
        }
    }
    for(int i = 1; i < nnets_; i++)   // insertion sort: at most 20
        for(int j = i; j > 0 && nets_[j].rssi > nets_[j - 1].rssi; j--) {
            const hal::WifiNetwork t = nets_[j];
            nets_[j] = nets_[j - 1];
            nets_[j - 1] = t;
        }
}

void WifiSetup::tick(uint32_t now)
{
    const hal::WifiLink link = wifi_.link();
    if(ap_on_) {
        st_.clients = wifi_.accessPointClients();
        pullScan();
    }

    switch(st_.phase) {
        case SetupPhase::Off:
            break;

        case SetupPhase::Hotspot:
        case SetupPhase::PhoneJoined:
            setPhase(st_.clients > 0 ? SetupPhase::PhoneJoined : SetupPhase::Hotspot);
            if(reached(now, setup_until_)) {
                closeAp();
                afterSetup(now);
            }
            break;

        case SetupPhase::Connecting:
            if(link == hal::WifiLink::Up) {
                copy(saved_ssid_, sizeof(saved_ssid_), try_ssid_);
                copy(saved_pw_, sizeof(saved_pw_), try_pw_);
                if(store_) {
                    store_->setStr(kKeySsid, saved_ssid_);
                    store_->setStr(kKeyPass, saved_pw_);
                    store_->commit();
                }
                st_.error = SetupError::None;
                retry_ms_ = cfg_.retry_min_ms;
                setPhase(SetupPhase::Connected);
                lingering_ = true;   // let the page show it worked, then close the setup network
                linger_until_ = now + cfg_.linger_ms;
            }
            else if(link == hal::WifiLink::WrongPassword || link == hal::WifiLink::NotFound ||
                    link == hal::WifiLink::Failed || reached(now, attempt_until_)) {
                st_.error = link == hal::WifiLink::Connecting ? SetupError::Failed : errorFor(link);
                wifi_.disconnect();
                setPhase(st_.clients > 0 ? SetupPhase::PhoneJoined : SetupPhase::Hotspot);
            }
            break;

        case SetupPhase::Connected:
            if(lingering_ && reached(now, linger_until_)) closeAp();
            if(link != hal::WifiLink::Up) {
                setPhase(SetupPhase::Offline);
                retry_ms_ = cfg_.retry_min_ms;
                retry_at_ = now + retry_ms_;
            }
            break;

        case SetupPhase::Offline:
            if(link == hal::WifiLink::Up) {
                setPhase(SetupPhase::Connected);
                retry_ms_ = cfg_.retry_min_ms;
            }
            else if(link != hal::WifiLink::Connecting && reached(now, retry_at_)) {
                wifi_.connect(saved_ssid_, saved_pw_);
                retry_at_ = now + retry_ms_;
                retry_ms_ = retry_ms_ * 2u > cfg_.retry_max_ms ? cfg_.retry_max_ms : retry_ms_ * 2u;
            }
            break;
    }

    st_.setup_left_ms = setupOpen() && !reached(now, setup_until_) ? setup_until_ - now : 0;
    const uint32_t a = link == hal::WifiLink::Up ? wifi_.stationAddress() : 0u;
    if(a) std::snprintf(st_.address, sizeof(st_.address), "%u.%u.%u.%u", static_cast<unsigned>(a >> 24),
                        static_cast<unsigned>((a >> 16) & 0xFFu), static_cast<unsigned>((a >> 8) & 0xFFu),
                        static_cast<unsigned>(a & 0xFFu));
    else st_.address[0] = '\0';
}

}  // namespace brew::net
