/* The setup network's DNS answers (lib/network/captive_dns): byte-exact replies to queries
 * shaped like the ones phones send. */
#include "brew/network/captive_dns.h"
#include "minitest.h"

#include <string>
#include <vector>

using namespace brew::net;

namespace {

const uint8_t kIp[4] = { 192, 168, 4, 1 };
const uint8_t kPhone[4] = { 192, 168, 4, 2 };

/* A query as Android sends it: id, RD set, one question, plus an EDNS (OPT) record. */
std::vector<uint8_t> query(const std::string & name, uint16_t type, bool edns = true, uint16_t id = 0x1234)
{
    std::vector<uint8_t> q = { static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id), 0x01, 0x00,   // RD
                               0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, static_cast<uint8_t>(edns ? 1 : 0) };
    size_t at = 0;
    while(at <= name.size()) {
        size_t dot = name.find('.', at);
        if(dot == std::string::npos) dot = name.size();
        q.push_back(static_cast<uint8_t>(dot - at));
        for(size_t i = at; i < dot; i++) q.push_back(static_cast<uint8_t>(name[i]));
        at = dot + 1;
    }
    q.push_back(0);
    q.push_back(static_cast<uint8_t>(type >> 8));
    q.push_back(static_cast<uint8_t>(type));
    q.push_back(0x00);
    q.push_back(0x01);   // class IN
    if(edns) for(uint8_t b : { 0x00, 0x00, 0x29, 0x05, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }) q.push_back(b);
    return q;
}

}  // namespace

TEST(captive_dns_answers_a_queries_with_the_device)
{
    const auto q = query("connectivitycheck.gstatic.com", 1);
    uint8_t out[512];
    DnsQuestion dq;
    const size_t n = captiveDnsAnswer(q.data(), q.size(), kPhone, kIp, 30, out, sizeof(out), &dq);
    CHECK_EQ(std::string(dq.name), std::string("connectivitycheck.gstatic.com"));
    CHECK_EQ(dq.type, 1);
    const size_t qlen = q.size() - 11;   // header + question, without the OPT record
    CHECK_EQ(n, qlen + 16);
    CHECK_EQ(out[0], 0x12); CHECK_EQ(out[1], 0x34);   // same id
    CHECK_EQ(out[2], 0x81); CHECK_EQ(out[3], 0x80);   // response, RD echoed, RA, no error
    CHECK_EQ(out[5], 1); CHECK_EQ(out[7], 1);         // 1 question, 1 answer
    CHECK_EQ(out[9], 0); CHECK_EQ(out[11], 0);        // no authority / additional (EDNS dropped)
    for(size_t i = 12; i < qlen; i++) CHECK_EQ(out[i], q[i]);   // the question, echoed
    const uint8_t ans[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
    for(size_t i = 0; i < 16; i++) CHECK_EQ(out[qlen + i], ans[i]);
}

TEST(captive_dns_aaaa_gets_an_empty_answer_so_phones_use_ipv4)
{
    const auto q = query("captive.apple.com", 28);
    uint8_t out[512];
    const size_t n = captiveDnsAnswer(q.data(), q.size(), kPhone, kIp, 30, out, sizeof(out));
    CHECK_EQ(n, q.size() - 11);
    CHECK_EQ(out[7], 0);    // no answers
    CHECK_EQ(out[3], 0x80); // and no error
}

TEST(captive_dns_ignores_what_it_should)
{
    uint8_t out[512];
    auto q = query("example.com", 1);
    const uint8_t elsewhere[4] = { 10, 0, 0, 7 };
    CHECK_EQ(captiveDnsAnswer(q.data(), q.size(), elsewhere, kIp, 30, out, sizeof(out)), 0u);   // not the setup network
    q[2] |= 0x80;   // a response
    CHECK_EQ(captiveDnsAnswer(q.data(), q.size(), kPhone, kIp, 30, out, sizeof(out)), 0u);
    q = query("example.com", 1);
    CHECK_EQ(captiveDnsAnswer(q.data(), 11, kPhone, kIp, 30, out, sizeof(out)), 0u);            // short header
    CHECK_EQ(captiveDnsAnswer(q.data(), 20, kPhone, kIp, 30, out, sizeof(out)), 0u);            // cut-off name
    q[12] = 0xC0;   // a compression pointer where the name should be
    CHECK_EQ(captiveDnsAnswer(q.data(), q.size(), kPhone, kIp, 30, out, sizeof(out)), 0u);
    q = query("example.com", 1, false);
    CHECK(captiveDnsAnswer(q.data(), q.size(), kPhone, kIp, 30, out, sizeof(out)) > 0);           // plain query, no EDNS
}
