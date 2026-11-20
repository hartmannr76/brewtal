/**
 * PRIVATE to brew_core/src/codec.cpp. Do not include from anywhere else (tools/check_rules.py
 * enforces this). Wire format version 1: explicit little-endian fields, written one by one.
 * No memcpy of structs, no packed structs, no bitfields, no floats on the wire.
 *
 * The `#n` numbers are field numbers: keep them identical to proto/brewdial.proto so a
 * future nanopb codec (proto_ver 2) maps 1:1. Evolution rule inside v1: only APPEND new
 * fields at the end of a payload; never reorder, resize or remove. Decoders ignore extra
 * trailing bytes, and treat missing trailing fields as defaults.
 *
 * State (msg_type 1)
 *   #1 temp_cc          i16   centi-degC
 *   #2 setpoint_ferm_cc i16
 *   #3 setpoint_crash_cc i16
 *   #4 mode             u8    0 ferment, 1 crash
 *   #5 action           u8    0 idle, 1 cooling, 2 heating
 *   #6 flags            u8    bit0 paused, bit1 celsius, bit2 temp_valid
 *   #7 day              u16
 *   #8 batch            u8 len + len bytes (UTF-8, len <= 15)
 *   #9 wanted           u8    0 idle, 1 cooling, 2 heating (missing: same as action)
 *   #10 wait_s          u16   seconds until wanted may start (missing: 0)
 *
 * Command (msg_type 2)
 *   #1 kind             u8    1 set_setpoint, 2 set_mode, 3 set_units, 4 request_sync
 *   #2 mode             u8
 *   #3 setpoint_cc      i16
 *   #4 flags            u8    bit0 paused, bit1 celsius
 *
 * HistoryChunk (msg_type 3)
 *   #1 flags            u8    bit0 reset, bit1 more chunks follow
 *   #2 minutes_per_sample u8  rounded; 0 under 30 s (for decoders that predate #5)
 *   #3 n                u8    <= 24
 *   #4 temps_cc         i16 x n
 *   #5 seconds_per_sample u16 (missing: minutes_per_sample x 60)
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace brew::codec::v1 {

class Writer {
public:
    Writer(uint8_t * buf, size_t max) : buf_(buf), max_(max) {}

    void u8(uint8_t v)
    {
        if(pos_ + 1 > max_) { ok_ = false; return; }
        buf_[pos_++] = v;
    }
    void u16(uint16_t v)
    {
        u8(static_cast<uint8_t>(v & 0xFF));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void bytes(const uint8_t * p, size_t n)
    {
        for(size_t i = 0; i < n; i++) u8(p[i]);
    }

    bool ok() const { return ok_; }
    size_t size() const { return ok_ ? pos_ : 0; }

private:
    uint8_t * buf_;
    size_t max_;
    size_t pos_ = 0;
    bool ok_ = true;
};

class Reader {
public:
    Reader(const uint8_t * buf, size_t n) : buf_(buf), n_(n) {}

    /* Missing trailing fields read as 0 and leave ok() true: older senders are allowed to
     * omit fields that were appended later. Use has() before optional fields. */
    bool has(size_t k) const { return pos_ + k <= n_; }
    uint8_t u8() { return has(1) ? buf_[pos_++] : 0; }
    uint16_t u16()
    {
        const uint16_t lo = u8();
        const uint16_t hi = u8();
        return static_cast<uint16_t>(lo | (hi << 8));
    }
    int16_t i16() { return static_cast<int16_t>(u16()); }
    size_t pos() const { return pos_; }

private:
    const uint8_t * buf_;
    size_t n_;
    size_t pos_ = 0;
};

inline int16_t to_cc(float c)
{
    const long v = std::lround(c * 100.0f);
    if(v > INT16_MAX) return INT16_MAX;
    if(v < INT16_MIN) return INT16_MIN;
    return static_cast<int16_t>(v);
}

inline float from_cc(int16_t v) { return static_cast<float>(v) / 100.0f; }

}  // namespace brew::codec::v1
