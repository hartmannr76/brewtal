/**
 * The setup network's DNS answers, as a pure function so it is unit-tested on the host (the
 * ESP32 driver only moves UDP packets). Every name resolves to the device, so a phone's
 * "is there internet?" check lands on the setup page:
 *   - A queries: one answer, `ip`
 *   - AAAA and anything else: no answer (no error), so the phone falls back to A
 *   - queries from outside the setup network's /24, responses, malformed packets: ignored
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace brew::net {

struct DnsQuestion {
    char name[96] = { 0 };   // "connectivitycheck.gstatic.com" (cut if longer)
    int type = 0;            // 1 = A, 28 = AAAA, ...
};

/**
 * Builds the answer to `query` (from `src_ip`, both in network byte order as bytes a.b.c.d)
 * into `out`. Returns its length, or 0 to send nothing. `q` (optional) gets the question.
 */
size_t captiveDnsAnswer(const uint8_t * query, size_t n, const uint8_t src_ip[4], const uint8_t ip[4],
                        uint32_t ttl_s, uint8_t * out, size_t cap, DnsQuestion * q = nullptr);

}  // namespace brew::net
