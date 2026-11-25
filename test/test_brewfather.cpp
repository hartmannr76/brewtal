/* Brewfather logging and account checks, against a fake HTTP client. */
#include "brew/network/brewfather.h"
#include "minitest.h"

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace brew;
using namespace brew::net;

namespace {

struct FakeHttp final : hal::HttpClient {
    struct Req {
        std::string method, url, auth, type, body;
    };
    std::vector<Req> reqs;
    bool busy = false, done = false;
    int status = 0;
    std::string body;

    bool begin() override { return true; }
    bool start(const hal::HttpClientRequest & r) override
    {
        if(busy) return false;
        reqs.push_back({ r.method, r.url, r.authorization ? r.authorization : "", r.content_type ? r.content_type : "",
                         std::string(r.body, r.body_len) });
        busy = true;
        done = false;
        return true;
    }
    bool poll(hal::HttpClientResult & out) override
    {
        if(!busy || !done) return false;
        out.status = status;
        out.body = body.c_str();
        out.body_len = body.size();
        busy = done = false;
        return true;
    }
    void respond(int s, const std::string & b = "")
    {
        status = s;
        body = b;
        done = true;
    }
};

struct Store final : hal::SettingsStore {
    std::map<std::string, std::string> s;
    bool getI32(const char *, int32_t &) override { return false; }
    bool setI32(const char *, int32_t) override { return true; }
    bool getStr(const char * k, char * out, size_t max) override
    {
        auto it = s.find(k);
        if(it == s.end()) return false;
        std::snprintf(out, max, "%s", it->second.c_str());
        return true;
    }
    bool setStr(const char * k, const char * v) override { s[k] = v; return true; }
    bool commit() override { return true; }
};

/* a Fermenting batch with our device attached, as Brewfather lists it (include=devices) */
const char * kAttached =
    "[{\"_id\":\"ZEo\",\"batchNo\":18,\"name\":\"Batch\",\"recipe\":{\"name\":\"Pale Ale\"},\"status\":\"Fermenting\","
    "\"devices\":{\"tilt\":{\"enabled\":true,\"items\":[]},\"stream\":{\"enabled\":true,\"items\":["
    "{\"name\":\"BrewDialCB50\",\"key\":\"BREWDIALCB50\",\"type\":\"stream\",\"lastLog\":1795628107772}]}}}]";
const char * kNotAttached =
    "[{\"_id\":\"ZEo\",\"batchNo\":18,\"name\":\"Batch\",\"recipe\":{\"name\":\"Pale Ale\"},"
    "\"devices\":{\"stream\":{\"enabled\":true,\"items\":[{\"name\":\"Other\",\"key\":\"OTHER\"}]}}}]";

void run(Brewfather & bf, FakeHttp & http, uint32_t & now, uint32_t ms, bool online = true)
{
    for(uint32_t end = now + ms; now < end;) {
        now += 100;
        bf.tick(now, online, 0);
        (void)http;
    }
}

}  // namespace

TEST(brewfather_base64)
{
    char b[16];
    CHECK(Brewfather::base64("user:key", 8, b, sizeof(b)));
    CHECK(std::string(b) == "dXNlcjprZXk=");
    CHECK(Brewfather::base64("ab", 2, b, sizeof(b)) && std::string(b) == "YWI=");
    CHECK(Brewfather::base64("abc", 3, b, sizeof(b)) && std::string(b) == "YWJj");
    CHECK(!Brewfather::base64("abcdefghijklmnop", 16, b, sizeof(b)));   // too small: refused, not overrun
}

TEST(brewfather_checks_the_account_and_finds_the_batch)
{
    FakeHttp http;
    Store store;
    Brewfather bf(http, &store);
    uint32_t now = 1000;
    bf.begin("BrewDial-CB50", now);
    CHECK(std::string(bf.status().device) == "BrewDialCB50");   // letters and digits only
    run(bf, http, now, 5000);
    CHECK(bf.status().account == BfAccount::NotSetUp);
    CHECK(http.reqs.empty());                                    // nothing saved: no calls

    bf.setCredentials("uid", "secret", "http://log.brewfather.net/stream?id=L31X", now);
    CHECK(store.s["bf_stream"] == "L31X");                       // the id, from a pasted URL
    CHECK(bf.status().user_set && bf.status().key_set && bf.status().stream_set);
    CHECK(bf.status().account == BfAccount::Checking);
    run(bf, http, now, 100, false);
    CHECK(http.reqs.empty());                                    // offline: waits
    run(bf, http, now, 100);
    CHECK_EQ(http.reqs.size(), static_cast<size_t>(1));
    CHECK(http.reqs[0].method == "GET");
    CHECK(http.reqs[0].url == "https://api.brewfather.app/v2/batches?status=Fermenting&include=devices&limit=50");
    CHECK(http.reqs[0].auth == "Basic dWlkOnNlY3JldA==");        // base64("uid:secret")
    http.respond(200, kAttached);
    run(bf, http, now, 100);
    CHECK(bf.status().account == BfAccount::Connected);
    CHECK(bf.status().attached);
    CHECK(std::string(bf.status().batch) == "#18 Pale Ale");
    CHECK(bf.status().last_log_unix_ms == 1795628107772LL);
    CHECK_EQ(bf.lastLogAgeS(1795628107772LL + 180000), 180);
    CHECK_EQ(bf.lastLogAgeS(0), -1);

    /* the next check (30 min later) finds it detached */
    run(bf, http, now, 30u * 60u * 1000u);
    CHECK_EQ(http.reqs.size(), static_cast<size_t>(2));
    http.respond(200, kNotAttached);
    run(bf, http, now, 100);
    CHECK(!bf.status().attached);
    CHECK(bf.status().batch[0] == '\0');

    /* a wrong key */
    bf.setCredentials(nullptr, "wrong", nullptr, now);
    CHECK(store.s["bf_user"] == "uid");                          // empty fields keep what's saved
    run(bf, http, now, 100);
    http.respond(401);
    run(bf, http, now, 100);
    CHECK(bf.status().account == BfAccount::Rejected);
    CHECK_EQ(bf.status().http, 401);

    /* no answer: Unreachable, retried after 5 min, what we knew is kept */
    bf.setCredentials(nullptr, "secret", nullptr, now);
    run(bf, http, now, 100);
    http.respond(200, kAttached);
    run(bf, http, now, 100);
    bf.checkNow(now);
    run(bf, http, now, 100);
    http.respond(-1);
    run(bf, http, now, 100);
    CHECK(bf.status().account == BfAccount::Unreachable);
    CHECK(bf.status().attached);
    const size_t before = http.reqs.size();
    run(bf, http, now, 4u * 60u * 1000u);
    CHECK_EQ(http.reqs.size(), before);
    run(bf, http, now, 60u * 1000u + 200u);
    CHECK_EQ(http.reqs.size(), before + 1);
}

TEST(brewfather_posts_every_15_minutes_and_checks_it_landed)
{
    FakeHttp http;
    Store store;
    Brewfather bf(http, &store);
    uint32_t now = 1000;
    bf.begin("BrewDialCB50", now);
    bf.setCredentials(nullptr, nullptr, "L31X", now);            // logging needs only the stream id
    CHECK(bf.status().account == BfAccount::NotSetUp);
    run(bf, http, now, 60000);
    CHECK(http.reqs.empty());                                    // no reading yet: nothing to post
    bf.setReading(true, 19.44f, 20.0f, "heating");
    run(bf, http, now, 100);
    CHECK_EQ(http.reqs.size(), static_cast<size_t>(1));
    CHECK(http.reqs[0].method == "POST");
    CHECK(http.reqs[0].url == "https://log.brewfather.net/stream?id=L31X");
    CHECK(http.reqs[0].type == "application/json");
    CHECK(http.reqs[0].body ==
          "{\"name\":\"BrewDialCB50\",\"temp\":19.44,\"temp_unit\":\"C\",\"temp_target\":20.00,\"device_state\":\"heating\"}");
    CHECK(http.reqs[0].auth.empty());
    http.respond(200, "{ \"result\": \"OK\" }");
    run(bf, http, now, 100);
    CHECK_EQ(bf.status().last_post_http, 200);
    CHECK(bf.status().last_post_ms != 0);

    /* never twice within 15 minutes (Brewfather would ignore it) */
    run(bf, http, now, 15u * 60u * 1000u);
    CHECK_EQ(http.reqs.size(), static_cast<size_t>(1));
    run(bf, http, now, 20000);
    CHECK_EQ(http.reqs.size(), static_cast<size_t>(2));
    http.respond(200);
    run(bf, http, now, 100);

    /* with an account, a check follows 90 s after each post */
    bf.setCredentials("uid", "secret", nullptr, now);
    run(bf, http, now, 100);
    http.respond(200, kNotAttached);                             // the check right after saving
    run(bf, http, now, 15u * 60u * 1000u);
    const size_t n = http.reqs.size();
    CHECK(http.reqs[n - 1].method == "POST");
    http.respond(200);
    run(bf, http, now, 85000);
    CHECK_EQ(http.reqs.size(), n);
    run(bf, http, now, 6000);
    CHECK_EQ(http.reqs.size(), n + 1);
    CHECK(http.reqs[n].method == "GET");
    http.respond(200, kAttached);
    run(bf, http, now, 100);
    CHECK(bf.status().attached);

    /* an invalid reading (no probe) is not posted */
    bf.setReading(false, 0.0f, 20.0f, "idle");
    const size_t m = http.reqs.size();
    run(bf, http, now, 20u * 60u * 1000u);
    for(size_t i = m; i < http.reqs.size(); i++) CHECK(http.reqs[i].method != "POST");
}

TEST(brewfather_remembers_and_forgets)
{
    FakeHttp http;
    Store store;
    uint32_t now = 1000;
    {
        Brewfather bf(http, &store);
        bf.begin("BrewDialCB50", now);
        bf.setCredentials("uid", "secret", "L31X", now);
    }
    Brewfather again(http, &store);
    again.begin("BrewDialCB50", now);
    CHECK(again.status().user_set && again.status().key_set && again.status().stream_set);
    CHECK(again.status().account == BfAccount::Checking);
    again.forget();
    CHECK(again.status().account == BfAccount::NotSetUp);
    CHECK(!again.status().key_set);
    CHECK(std::string(again.status().device) == "BrewDialCB50");
    CHECK(store.s["bf_key"].empty() && store.s["bf_user"].empty() && store.s["bf_stream"].empty());
    again.setReading(true, 20.0f, 20.0f, "idle");
    run(again, http, now, 60u * 60u * 1000u);
    CHECK(http.reqs.empty());
}
