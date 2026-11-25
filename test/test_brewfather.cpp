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
    run(bf, http, now, 60000);
    CHECK(http.reqs.empty());                                    // no batch imported: nothing to post
    CHECK(bf.status().log == BfLog::Off);
    bf.startLogging("ZEo", "#18 Pale Ale", now);                 // "Run this profile" on the dial
    CHECK(bf.status().log == BfLog::NotAttached);
    CHECK(store.s["bf_batch"] == "ZEo" && store.s["bf_blabel"] == "#18 Pale Ale");
    run(bf, http, now, 19000);
    CHECK(http.reqs.empty());
    run(bf, http, now, 1100);
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
    CHECK(bf.status().log == BfLog::Attached);                   // to the imported batch

    /* attached to a different batch: stops posting (readings would land there), says where */
    bf.checkNow(now);
    run(bf, http, now, 100);
    std::string other = kAttached;
    other.replace(other.find("\"ZEo\""), 5, "\"Q9x\"");
    other.replace(other.find("18"), 2, "21");
    http.respond(200, other);
    run(bf, http, now, 100);
    CHECK(bf.status().log == BfLog::Elsewhere);
    CHECK(std::string(bf.status().elsewhere) == "#21 Pale Ale");
    const size_t e = http.reqs.size();
    run(bf, http, now, 20u * 60u * 1000u);
    for(size_t i = e; i < http.reqs.size(); i++) CHECK(http.reqs[i].method != "POST");
    /* re-attached in Brewfather: the next check finds it and posting resumes */
    bf.checkNow(now);
    run(bf, http, now, 100);
    http.respond(200, kAttached);
    run(bf, http, now, 100);
    CHECK(bf.status().log == BfLog::Attached);
    const size_t r = http.reqs.size();
    run(bf, http, now, 16u * 60u * 1000u);
    CHECK(http.reqs.size() > r && http.reqs[r].method == "POST");
    http.respond(200);
    run(bf, http, now, 100);

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
        bf.startLogging("ZEo", "#18 Pale Ale", now);
    }
    Brewfather again(http, &store);
    again.begin("BrewDialCB50", now);
    CHECK(again.status().user_set && again.status().key_set && again.status().stream_set);
    CHECK(again.status().account == BfAccount::Checking);
    CHECK(std::string(again.status().logging_id) == "ZEo");      // still logging to the batch after a restart
    CHECK(std::string(again.status().logging) == "#18 Pale Ale");
    CHECK(again.status().log == BfLog::NotAttached);              // until a check says where it lands
    again.forget();
    CHECK(again.status().log == BfLog::Off);
    CHECK(store.s["bf_batch"].empty());
    CHECK(again.status().account == BfAccount::NotSetUp);
    CHECK(!again.status().key_set);
    CHECK(std::string(again.status().device) == "BrewDialCB50");
    CHECK(store.s["bf_key"].empty() && store.s["bf_user"].empty() && store.s["bf_stream"].empty());
    again.setReading(true, 20.0f, 20.0f, "idle");
    run(again, http, now, 60u * 60u * 1000u);
    CHECK(http.reqs.empty());
}

TEST(brewfather_logging_prefers_the_running_batch)
{
    FakeHttp http;
    Store store;
    store.s["bf_batch"] = "ZEo";                 // imported before labels were saved
    Brewfather bf(http, &store);
    uint32_t now = 1000;
    bf.begin("BrewDialCB50", now);
    bf.setCredentials("uid", "secret", "L31X", now);
    run(bf, http, now, 100);
    /* attached to #21 and to #18 (the one running): it's logging to #18 */
    std::string other = kAttached;
    other.replace(other.find("\"ZEo\""), 5, "\"Q9x\"");
    other.replace(other.find("18"), 2, "21");
    const std::string both = other.substr(0, other.size() - 1) + "," + std::string(kAttached).substr(1);
    http.respond(200, both);
    run(bf, http, now, 100);
    CHECK(bf.status().log == BfLog::Attached);
    CHECK(std::string(bf.status().batch) == "#18 Pale Ale");
    CHECK(std::string(bf.status().logging) == "#18 Pale Ale");   // the label, from the list
}

TEST(brewfather_short_names)
{
    char b[16];
    Brewfather::shortName("Batch", "(Modified) GBB Gluten-free Pale Ale", b, sizeof(b));
    CHECK(std::string(b) == "GBB Gluten-free");          // 15 bytes, no trailing space
    Brewfather::shortName("B\xC3\xA4rktoberfest", "GF M\xC3\xA4rzen", b, sizeof(b));
    CHECK(std::string(b) == "B\xC3\xA4rktoberfest");     // the batch's own name wins
    Brewfather::shortName("", "Pale Ale", b, sizeof(b));
    CHECK(std::string(b) == "Pale Ale");
    Brewfather::shortName("Batch", "12345678901234\xC3\xA4", b, sizeof(b));
    CHECK(std::string(b) == "12345678901234");           // never half a character
}

TEST(brewfather_fetches_a_batch_for_import)
{
    FakeHttp http;
    Store store;
    Brewfather bf(http, &store);
    uint32_t now = 1000;
    bf.begin("BrewDialCB50", now);
    bf.fetchBatch(0, now);
    CHECK(bf.status().preview.state == BfPreview::State::Failed);   // nothing listed yet
    bf.setCredentials("uid", "secret", nullptr, now);
    run(bf, http, now, 100);
    http.respond(200,
                 "[{\"_id\":\"ZEo\",\"batchNo\":18,\"name\":\"Batch\",\"recipe\":{\"name\":\"(Modified) GBB Gluten-free Pale Ale\"},"
                 "\"status\":\"Fermenting\"},{\"_id\":\"k3J\",\"batchNo\":19,\"name\":\"B\u00e4rktoberfest\",\"recipe\":{\"name\":\"x\"}}]");
    run(bf, http, now, 100);
    CHECK_EQ(bf.status().nbatches, 2);
    CHECK(std::string(bf.status().batches[0].label) == "#18 GBB Gluten-free Pale Ale");
    CHECK(std::string(bf.status().batches[1].label) == "#19 B\xC3\xA4rktoberfest");
    CHECK(std::string(bf.status().batches[0].id) == "ZEo");

    const uint32_t rev = bf.status().rev;
    bf.fetchBatch(0, now);
    CHECK(bf.status().preview.state == BfPreview::State::Loading);
    CHECK(bf.status().rev != rev);
    run(bf, http, now, 100);
    CHECK(http.reqs.back().url == "https://api.brewfather.app/v2/batches/ZEo");
    CHECK(http.reqs.back().auth == "Basic dWlkOnNlY3JldA==");
    /* trimmed from the real batch: Primary 20 C 7 days, then a 1-day ramp to 11.67 C held 14 days */
    http.respond(200,
                 "{\"_id\":\"ZEo\",\"name\":\"Batch\",\"batchNo\":18,\"fermentationStartDate\":1795582800000,"
                 "\"recipe\":{\"name\":\"(Modified) GBB Gluten-free Pale Ale\",\"fermentation\":{\"name\":\"Imported\",\"steps\":["
                 "{\"stepTime\":7,\"stepTemp\":20,\"displayStepTemp\":68,\"type\":\"Primary\",\"ramp\":null,\"actualTime\":1795582800000},"
                 "{\"ramp\":1,\"type\":\"Conditioning\",\"stepTemp\":11.666667,\"stepTime\":14,\"displayStepTemp\":53}]}},"
                 "\"status\":\"Fermenting\",\"_timestamp_ms\":1795627483611}");
    run(bf, http, now, 100);
    const BfPreview & p = bf.status().preview;
    CHECK(p.state == BfPreview::State::Ready);
    CHECK(std::string(p.label) == "#18 GBB Gluten-free Pale Ale");
    CHECK(std::string(p.name) == "GBB Gluten-free");
    CHECK_EQ(p.total_steps, 2);
    CHECK_EQ(p.nsteps, 2);
    CHECK(std::string(p.steps[0].type) == "Primary");
    CHECK_NEAR(p.steps[0].temp_c, 20.0, 1e-4);
    CHECK_EQ(p.steps[0].ramp_h, 0);
    CHECK_EQ(p.steps[0].hold_h, 168);
    CHECK_NEAR(p.steps[1].temp_c, 11.666667, 1e-4);
    CHECK_EQ(p.steps[1].ramp_h, 24);
    CHECK_EQ(p.steps[1].hold_h, 336);
    CHECK(p.start_unix_ms == 1795582800000LL);
    CHECK(p.modified_unix_ms == 1795627483611LL);

    /* a long profile: the first 8 steps, and how many there were */
    bf.fetchBatch(1, now);
    run(bf, http, now, 100);
    std::string steps;
    for(int i = 0; i < 10; i++) steps += std::string(i ? "," : "") + "{\"type\":\"Primary\",\"stepTemp\":" + std::to_string(10 + i) + ",\"stepTime\":1}";
    http.respond(200, "{\"name\":\"x\",\"recipe\":{\"fermentation\":{\"steps\":[" + steps + "]}}}");
    run(bf, http, now, 100);
    CHECK(bf.status().preview.state == BfPreview::State::Ready);
    CHECK_EQ(bf.status().preview.total_steps, 10);
    CHECK_EQ(bf.status().preview.nsteps, 8);
    CHECK_NEAR(bf.status().preview.steps[7].temp_c, 17.0, 1e-4);

    /* gone in Brewfather */
    bf.fetchBatch(0, now);
    run(bf, http, now, 100);
    http.respond(404);
    run(bf, http, now, 100);
    CHECK(bf.status().preview.state == BfPreview::State::Failed);
}
