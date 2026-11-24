#include "brew/network/captive_dns.h"

#include <cstring>

namespace brew::net {

size_t captiveDnsAnswer(const uint8_t * in, size_t n, const uint8_t src[4], const uint8_t ip[4], uint32_t ttl,
                        uint8_t * out, size_t cap, DnsQuestion * q)
{
    if(!in || !out || n < 12 || n > cap) return 0;                       // shorter than a header
    if(src[0] != ip[0] || src[1] != ip[1] || src[2] != ip[2]) return 0;  // not from the setup network
    if(in[2] & 0x80) return 0;                                           // a response, not a query
    if(((in[2] >> 3) & 0x0F) != 0) return 0;                             // not a standard query
    if(((in[4] << 8) | in[5]) < 1) return 0;                             // no question

    /* the first question: labels up to a zero byte, then type (2) and class (2) */
    size_t p = 12;
    DnsQuestion tmp;
    DnsQuestion & qq = q ? *q : tmp;
    size_t o = 0;
    while(p < n && in[p] != 0) {
        const size_t l = in[p];
        if(l > 63 || p + 1 + l > n) return 0;   // a compression pointer or garbage: not a plain question
        if(o && o + 1 < sizeof(qq.name)) qq.name[o++] = '.';
        for(size_t k = 0; k < l && o + 1 < sizeof(qq.name); k++) qq.name[o++] = static_cast<char>(in[p + 1 + k]);
        p += 1 + l;
    }
    qq.name[o] = '\0';
    if(p + 5 > n) return 0;
    p += 1;   // the zero byte
    qq.type = (in[p] << 8) | in[p + 1];
    const size_t qend = p + 4;
    const bool a = qq.type == 1;
    if(qend + (a ? 16u : 0u) > cap) return 0;

    std::memcpy(out, in, qend);    // id, flags, counts, the question
    out[2] = static_cast<uint8_t>(0x80 | (in[2] & 0x01));   // response; recursion-desired echoed
    out[3] = 0x80;                 // recursion available, no error
    out[4] = 0;
    out[5] = 1;                    // one question (the first; any others are dropped)
    out[6] = 0;
    out[7] = a ? 1 : 0;
    out[8] = out[9] = out[10] = out[11] = 0;   // no authority or additional records (drops EDNS)
    size_t len = qend;
    if(a) {
        const uint8_t ans[16] = { 0xC0, 0x0C,               // name: points at the question
                                  0x00, 0x01, 0x00, 0x01,   // type A, class IN
                                  static_cast<uint8_t>(ttl >> 24), static_cast<uint8_t>(ttl >> 16),
                                  static_cast<uint8_t>(ttl >> 8), static_cast<uint8_t>(ttl),
                                  0x00, 0x04, ip[0], ip[1], ip[2], ip[3] };
        std::memcpy(out + len, ans, sizeof(ans));
        len += sizeof(ans);
    }
    return len;
}

}  // namespace brew::net
