/**
 * A UDP port answered by a function: each datagram goes to `answer`, and whatever it returns is
 * sent back to the sender. Runs in its own task. Used for the setup network's DNS (port 53),
 * with the answers built by portable, host-tested code (lib/network/captive_dns).
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace brew::drivers {

class EspUdpResponder {
public:
    /** Returns the reply's length in `out` (at most `cap`), or 0 to send nothing.
     *  `src` is the sender's IPv4 address, a.b.c.d. */
    using Answer = size_t (*)(void * ctx, const uint8_t * in, size_t n, const uint8_t src[4], uint8_t * out, size_t cap);

    EspUdpResponder(uint16_t port, Answer answer, void * ctx) : port_(port), answer_(answer), ctx_(ctx) {}

    bool begin();

    /* the task body */
    void run();

private:
    uint16_t port_;
    Answer answer_;
    void * ctx_;
    int sock_ = -1;
};

}  // namespace brew::drivers
