/* WiFi setup (lib/network): the setup flow against a scripted radio, and the setup page's
 * HTTP handler. */
#include "brew/network/setup_portal.h"
#include "brew/network/wifi_setup.h"
#include "minitest.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace brew;
using namespace brew::net;

namespace {

/* A radio the test drives by hand: what connect() leads to is set with `next`/`link_`. */
struct FakeWifi final : hal::Wifi {
    bool ap = false;
    std::string ap_ssid, ap_pw;
    int clients = 0;
    bool scanning = false;
    std::vector<hal::WifiNetwork> found;
    hal::WifiLink link_ = hal::WifiLink::Down;
    std::string tried_ssid, tried_pw;
    int connects = 0, disconnects = 0, scans = 0;

    bool begin() override { return true; }
    uint32_t deviceId() override { return 0xA14F2Au; }
    uint32_t random() override { return 12345u; }
    bool startAccessPoint(const char * s, const char * p) override { ap = true; ap_ssid = s; ap_pw = p; return true; }
    void stopAccessPoint() override { ap = false; clients = 0; }
    int accessPointClients() override { return clients; }
    bool startScan() override { scanning = true; scans++; return true; }
    int scanResults(hal::WifiNetwork * out, int max) override
    {
        if(scanning) return -1;
        int n = 0;
        for(const auto & f : found) if(n < max) out[n++] = f;
        return n;
    }
    bool connect(const char * s, const char * p) override
    {
        tried_ssid = s; tried_pw = p; connects++;
        link_ = hal::WifiLink::Connecting;
        return true;
    }
    void disconnect() override { link_ = hal::WifiLink::Down; disconnects++; }
    hal::WifiLink link() override { return link_; }
    uint32_t stationAddress() override { return link_ == hal::WifiLink::Up ? 0xC0A80105u : 0u; }   // 192.168.1.5
};

struct MemStore final : hal::SettingsStore {
    std::map<std::string, std::string> s;
    bool getI32(const char *, int32_t &) override { return false; }
    bool setI32(const char *, int32_t) override { return true; }
    bool getStr(const char * k, char * o, size_t m) override
    {
        auto it = s.find(k);
        if(it == s.end()) return false;
        std::snprintf(o, m, "%s", it->second.c_str());
        return true;
    }
    bool setStr(const char * k, const char * v) override { s[k] = v; return true; }
    bool commit() override { return true; }
};

hal::WifiNetwork ap(const char * ssid, int rssi, bool secured = true)
{
    hal::WifiNetwork n;
    std::snprintf(n.ssid, sizeof(n.ssid), "%s", ssid);
    n.rssi = static_cast<int8_t>(rssi);
    n.secured = secured;
    return n;
}

std::string get(SetupPortal & p, const char * method, const char * path, const char * body = "", uint32_t now = 0,
                int * status = nullptr)
{
    HttpRequest rq;
    rq.method = method;
    rq.path = path;
    rq.body = body;
    rq.body_len = std::strlen(body);
    HttpResponse rs;
    p.handle(rq, now, rs);
    if(status) *status = rs.status;
    return std::string(rs.body, rs.body_len);
}

}  // namespace

TEST(wifi_first_start_opens_setup_with_a_named_passworded_network)
{
    FakeWifi w;
    MemStore st;
    WifiSetup s(w, &st, WifiSetup::Config{});
    s.begin(0);
    s.tick(0);
    CHECK(s.status().phase == SetupPhase::Hotspot);
    CHECK(w.ap);
    CHECK_EQ(w.ap_ssid, std::string("BrewDial-4F2A"));   // low 16 bits of the device id
    CHECK(w.ap_pw.size() >= 8);                          // WPA2's minimum
    CHECK_EQ(std::string(s.status().ap_password), w.ap_pw);
    CHECK_EQ(w.scans, 1);

    /* a phone joins; the scan comes back sorted strongest first, one entry per name */
    w.clients = 1;
    w.found = { ap("NETGEAR27", -71), ap("Hartmann-5G", -60), ap("", -40), ap("Hartmann-5G", -48),
                ap("xfinitywifi", -82, false) };
    w.scanning = false;
    s.tick(100);
    CHECK(s.status().phase == SetupPhase::PhoneJoined);
    const hal::WifiNetwork * n;
    CHECK_EQ(s.networks(n), 3);
    CHECK_EQ(std::string(n[0].ssid), std::string("Hartmann-5G"));
    CHECK_EQ(static_cast<int>(n[0].rssi), -48);
    CHECK_EQ(std::string(n[2].ssid), std::string("xfinitywifi"));
}

TEST(wifi_wrong_password_keeps_setup_open_then_success_saves_and_closes_it)
{
    FakeWifi w;
    MemStore st;
    WifiSetup::Config cfg;
    cfg.linger_ms = 10000;
    WifiSetup s(w, &st, cfg);
    s.begin(0);
    w.clients = 1;
    s.tick(10);

    CHECK(s.submit("Hartmann-5G", "nope", 20));
    CHECK(s.status().phase == SetupPhase::Connecting);
    CHECK_EQ(w.tried_ssid, std::string("Hartmann-5G"));
    w.link_ = hal::WifiLink::WrongPassword;
    s.tick(2000);
    CHECK(s.status().phase == SetupPhase::PhoneJoined);
    CHECK(s.status().error == SetupError::WrongPassword);
    CHECK(w.ap);                                // still there to try again
    CHECK(st.s.find("wifi_ssid") == st.s.end());   // nothing saved

    CHECK(s.submit("Hartmann-5G", "hops2026", 3000));
    CHECK(s.status().error == SetupError::None);
    w.link_ = hal::WifiLink::Up;
    s.tick(5000);
    CHECK(s.status().phase == SetupPhase::Connected);
    CHECK_EQ(st.s["wifi_ssid"], std::string("Hartmann-5G"));
    CHECK_EQ(st.s["wifi_pass"], std::string("hops2026"));
    CHECK(w.ap);                                // lingers so the page can say it worked
    s.tick(14000);
    CHECK(w.ap);
    s.tick(15001);
    CHECK(!w.ap);
    CHECK(s.status().phase == SetupPhase::Connected);
}

TEST(wifi_a_connect_attempt_that_hangs_times_out)
{
    FakeWifi w;
    WifiSetup::Config cfg;
    cfg.connect_timeout_ms = 20000;
    WifiSetup s(w, nullptr, cfg);
    s.begin(0);
    s.submit("Basement", "password1", 0);
    s.tick(19999);
    CHECK(s.status().phase == SetupPhase::Connecting);
    s.tick(20000);
    CHECK(s.status().phase == SetupPhase::Hotspot);
    CHECK(s.status().error == SetupError::Failed);
}

TEST(wifi_saved_network_joins_quietly_and_retries_with_backoff)
{
    FakeWifi w;
    MemStore st;
    st.s["wifi_ssid"] = "Hartmann-5G";
    st.s["wifi_pass"] = "hops2026";
    WifiSetup::Config cfg;
    cfg.retry_min_ms = 5000;
    cfg.retry_max_ms = 20000;
    WifiSetup s(w, &st, cfg);
    s.begin(0);
    CHECK(!w.ap);                                     // no setup network
    CHECK(s.status().phase == SetupPhase::Offline);
    CHECK_EQ(w.connects, 1);
    w.link_ = hal::WifiLink::Up;
    s.tick(1000);
    CHECK(s.status().phase == SetupPhase::Connected);

    /* the router goes away: retries at 5, 10, 20, 20 s, and never opens setup */
    w.link_ = hal::WifiLink::NotFound;
    s.tick(2000);
    CHECK(s.status().phase == SetupPhase::Offline);
    const int c0 = w.connects;
    uint32_t t = 2000;
    std::vector<uint32_t> at;
    while(t < 2000 + 70000) {
        t += 100;
        s.tick(t);
        if(w.connects != static_cast<int>(c0 + at.size())) {
            at.push_back(t - 2000);
            w.link_ = hal::WifiLink::NotFound;   // each try fails
        }
    }
    CHECK(at.size() >= 4);
    CHECK_EQ(at[0], 5000u);
    CHECK_EQ(at[1] - at[0], 5000u);
    CHECK_EQ(at[2] - at[1], 10000u);
    CHECK_EQ(at[3] - at[2], 20000u);
    CHECK(!w.ap);

    w.link_ = hal::WifiLink::Up;
    s.tick(t + 100);
    CHECK(s.status().phase == SetupPhase::Connected);
}

TEST(wifi_setup_closes_on_skip_or_timeout_and_reopens_from_the_menu)
{
    FakeWifi w;
    WifiSetup::Config cfg;
    cfg.setup_timeout_ms = 60000;
    WifiSetup s(w, nullptr, cfg);
    s.begin(0);
    s.tick(59999);
    CHECK(s.status().phase == SetupPhase::Hotspot);
    CHECK_EQ(s.status().setup_left_ms, 1u);
    s.tick(60000);
    CHECK(s.status().phase == SetupPhase::Off);
    CHECK(!w.ap);

    s.startSetup(70000);
    CHECK(s.status().phase == SetupPhase::Hotspot);
    CHECK(w.ap);
    s.cancelSetup(71000);
    CHECK(s.status().phase == SetupPhase::Off);
    CHECK(!w.ap);
}

TEST(wifi_changing_networks_keeps_the_old_one_until_the_new_one_works)
{
    FakeWifi w;
    MemStore st;
    st.s["wifi_ssid"] = "Old";
    st.s["wifi_pass"] = "oldpass1";
    WifiSetup s(w, &st, WifiSetup::Config{});
    s.begin(0);
    w.link_ = hal::WifiLink::Up;
    s.tick(100);
    s.startSetup(200);                       // the WiFi menu item
    CHECK(s.status().phase == SetupPhase::Hotspot);
    CHECK(w.link_ == hal::WifiLink::Up);     // still online meanwhile
    s.submit("New", "newpass1", 300);
    w.link_ = hal::WifiLink::WrongPassword;
    s.tick(400);
    CHECK(s.status().error == SetupError::WrongPassword);
    s.cancelSetup(500);                      // gave up: back to the saved one
    CHECK(s.status().phase == SetupPhase::Offline);
    CHECK_EQ(std::string(s.status().ssid), std::string("Old"));
    s.tick(600);
    CHECK_EQ(w.tried_ssid, std::string("Old"));
    CHECK_EQ(st.s["wifi_ssid"], std::string("Old"));
}

TEST(wifi_password_words)
{
    char b[20];
    WifiSetup::makePassword(0, b, sizeof(b));
    CHECK_EQ(std::string(b), std::string("malt-0000"));
    WifiSetup::makePassword(12u * 7391u + 1u, b, sizeof(b));
    CHECK_EQ(std::string(b), std::string("hops-7391"));
}

TEST(portal_serves_the_page_state_and_redirects_probes)
{
    FakeWifi w;
    WifiSetup s(w, nullptr, WifiSetup::Config{});
    s.begin(0);
    w.found = { ap("Hartmann-5G", -48), ap("Caf\xC3\xA9 \"Bean\"", -70, false) };
    w.scanning = false;
    s.tick(10);
    SetupPortal p(s);

    int status = 0;
    std::string page = get(p, "GET", "/", "", 0, &status);
    CHECK_EQ(status, 200);
    CHECK(page.find("Choose your WiFi") != std::string::npos);

    std::string j = get(p, "GET", "/api/state");
    CHECK(j.find("\"phase\":\"hotspot\"") != std::string::npos);
    CHECK(j.find(",\"networks\":[{") != std::string::npos);   // nothing cut short
    CHECK(j.back() == '}');
    CHECK(j.find("\"device\":\"BrewDial-4F2A\"") != std::string::npos);
    CHECK(j.find("{\"ssid\":\"Hartmann-5G\",\"rssi\":-48,\"secured\":true}") != std::string::npos);
    CHECK(j.find("\"Caf\xC3\xA9 \\\"Bean\\\"\"") != std::string::npos);   // quotes escaped, UTF-8 kept

    HttpRequest rq;
    rq.path = "/generate_204";   // Android's "is there internet?" probe
    HttpResponse rs;
    p.handle(rq, 0, rs);
    CHECK_EQ(rs.status, 302);
    CHECK_EQ(std::string(rs.location), std::string("http://192.168.4.1/"));
}

TEST(portal_connect_decodes_the_form_and_reports_progress)
{
    FakeWifi w;
    WifiSetup s(w, nullptr, WifiSetup::Config{});
    s.begin(0);
    SetupPortal p(s);
    int status = 0;
    get(p, "POST", "/api/connect", "ssid=Hartmann+5G%21&password=p%26ss%3Dw0rd", 10, &status);
    CHECK_EQ(status, 200);
    CHECK_EQ(w.tried_ssid, std::string("Hartmann 5G!"));
    CHECK_EQ(w.tried_pw, std::string("p&ss=w0rd"));
    CHECK(get(p, "GET", "/api/state").find("\"phase\":\"connecting\"") != std::string::npos);

    w.link_ = hal::WifiLink::WrongPassword;
    s.tick(500);
    CHECK(get(p, "GET", "/api/state").find("\"error\":\"wrong_password\"") != std::string::npos);

    get(p, "POST", "/api/connect", "password=x", 600, &status);   // no network name
    CHECK_EQ(status, 400);

    get(p, "POST", "/api/skip", "", 700, &status);
    CHECK_EQ(status, 200);
    CHECK(s.status().phase == SetupPhase::Off);
    get(p, "POST", "/api/connect", "ssid=A&password=12345678", 800, &status);   // setup closed now
    CHECK_EQ(status, 400);
}

TEST(portal_form_field_edge_cases)
{
    char o[16];
    CHECK(SetupPortal::formField("a=1&ab=2&b=", 11, "ab", o, sizeof(o)) && std::string(o) == "2");
    CHECK(SetupPortal::formField("a=1&ab=2&b=", 11, "b", o, sizeof(o)) && std::string(o).empty());
    CHECK(!SetupPortal::formField("a=1&ab=2", 8, "c", o, sizeof(o)));
    CHECK(SetupPortal::formField("x=%4", 4, "x", o, sizeof(o)) && std::string(o) == "%4");   // truncated escape kept
    CHECK(SetupPortal::formField("x=0123456789abcdefghij", 22, "x", o, sizeof(o)) && std::strlen(o) == 15);
}
