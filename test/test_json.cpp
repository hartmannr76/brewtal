/* The JSON scanner, on the shapes Brewfather really returns (trimmed from device logs). */
#include "brew/network/json_scan.h"
#include "minitest.h"

#include <cstring>
#include <string>

using namespace brew::net;

namespace {
json::Value doc(const char * s) { return json::parse(s, std::strlen(s)); }
std::string str(json::Value v)
{
    char b[96];
    json::string(v, b, sizeof(b));
    return b;
}
}  // namespace

TEST(json_reads_a_batch_list)
{
    const char * body =
        "[{\"_id\":\"ZEofdo\",\"batchNo\":18,\"brewDate\":1795582800000,\"name\":\"Batch\","
        "\"recipe\":{\"name\":\"(Modified) GBB Gluten-free Pale Ale\"},\"status\":\"Fermenting\"},"
        " {\"_id\":\"k3Jt\",\"batchNo\":13,\"name\":\"B\\u00e4rktoberfest\",\"recipe\":{\"name\":\"GF M\xC3\xA4rzen\"},"
        "\"status\":\"Completed\"}]";
    const json::Value list = doc(body);
    CHECK(json::isArray(list));
    CHECK_EQ(json::size(list), static_cast<size_t>(2));
    const json::Value b0 = json::at(list, 0);
    CHECK(json::equals(json::get(b0, "_id"), "ZEofdo"));
    CHECK_NEAR(json::number(json::get(b0, "batchNo")), 18.0, 1e-9);
    CHECK_NEAR(json::number(json::get(b0, "brewDate")), 1795582800000.0, 1.0);   // Unix ms: past 2^32
    CHECK(str(json::get(json::get(b0, "recipe"), "name")) == "(Modified) GBB Gluten-free Pale Ale");
    const json::Value b1 = json::at(list, 1);
    CHECK(str(json::get(b1, "name")) == "B\xC3\xA4rktoberfest");                 // ä -> UTF-8
    CHECK(str(json::get(json::get(b1, "recipe"), "name")) == "GF M\xC3\xA4rzen");  // raw UTF-8 kept
    CHECK(!json::at(list, 2));
    CHECK(!json::get(b0, "missing"));
    CHECK_NEAR(json::number(json::get(b0, "missing"), -1.0), -1.0, 1e-9);
}

TEST(json_reads_steps_devices_and_nulls)
{
    const char * body =
        "{\"steps\":[{\"stepTime\":7,\"stepTemp\":20,\"ramp\":null,\"actualTime\":1795582800000},"
        "{\"ramp\":1,\"stepTemp\":11.666667,\"stepTime\":14}],"
        "\"devices\":{\"tilt\":{\"enabled\":true,\"items\":[]},"
        "\"stream\":{\"items\":[{\"name\":\"Brewdialtest\",\"key\":\"BREWDIALTEST\",\"lastLog\":1795628107772,"
        "\"lastData\":{\"temp\":20,\"status\":\"ignored\",\"comment\":\"a \\\"quoted\\\" \\\\ line\\n\"}}],\"enabled\":true}}}";
    const json::Value root = doc(body);
    CHECK(json::isObject(root));
    const json::Value steps = json::get(root, "steps");
    CHECK_EQ(json::size(steps), static_cast<size_t>(2));
    CHECK(json::isNull(json::get(json::at(steps, 0), "ramp")));
    CHECK_NEAR(json::number(json::get(json::at(steps, 1), "ramp")), 1.0, 1e-9);
    CHECK_NEAR(json::number(json::get(json::at(steps, 1), "stepTemp")), 11.666667, 1e-6);
    const json::Value items = json::get(json::get(json::get(root, "devices"), "stream"), "items");
    CHECK_EQ(json::size(items), static_cast<size_t>(1));
    const json::Value dev = json::at(items, 0);
    CHECK(json::equals(json::get(dev, "key"), "BREWDIALTEST"));
    CHECK_NEAR(json::number(json::get(dev, "lastLog")), 1795628107772.0, 1.0);
    CHECK(str(json::get(json::get(dev, "lastData"), "comment")) == "a \"quoted\" \\ line\n");
    CHECK_EQ(json::size(json::get(json::get(json::get(root, "devices"), "tilt"), "items")), static_cast<size_t>(0));
    CHECK_EQ(json::size(json::get(root, "devices")), static_cast<size_t>(2));
}

TEST(json_rejects_bad_input_and_cuts_strings_safely)
{
    CHECK(!doc(""));
    CHECK(!doc("{\"a\":1"));          // cut short (a truncated response)
    CHECK(!doc("{\"a\":1} trailing"));
    CHECK(!doc("{\"a\" 1}"));
    CHECK(!doc("[1,,2]"));
    CHECK(!json::parse(nullptr, 0));
    CHECK(doc("  {\"a\" : [ 1 , 2 ] }  "));
    std::string deep;
    for(int i = 0; i < 100; i++) deep += '[';
    for(int i = 0; i < 100; i++) deep += ']';
    CHECK(!doc(deep.c_str()));        // too deep: refused, not a stack overflow

    /* a cut never splits a UTF-8 character */
    const json::Value v = json::get(doc("{\"n\":\"\xC3\xA4\xC3\xA4\xC3\xA4\"}"), "n");
    char b[4];
    CHECK(json::string(v, b, sizeof(b)));
    CHECK_EQ(std::strlen(b), static_cast<size_t>(2));   // one "ä" fits, the second wouldn't
    CHECK(!json::string(json::get(doc("{\"n\":5}"), "n"), b, sizeof(b)));
    CHECK(b[0] == '\0');
    /* surrogate pair -> one 4-byte character */
    CHECK(str(json::get(doc("{\"e\":\"\\ud83c\\udf7a\"}"), "e")) == "\xF0\x9F\x8D\xBA");
}
