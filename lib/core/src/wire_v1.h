/**
 * PRIVATE to lib/core/src/codec.cpp. Do not include from anywhere else (tools/check_rules.py
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
 *   #1 kind             u8    1 set_setpoint, 2 set_mode, 3 set_units, 4 request_sync,
 *                             5 start_day_count, 6 set_tuning, 7 set_profile, 8 set_step,
 *                             9 set_step_count
 *   #2 mode             u8
 *   #3 setpoint_cc      i16
 *   #4 flags            u8    bit0 paused, bit1 celsius
 *   #5 tuning           u8    set_tuning: 0 heat_diff, 1 cool_diff, 2 heat_delay, 3 cool_delay, 4 crash_default
 *   #6 value            i32   set_tuning: centi-degC, or seconds for delays; set_profile: the
 *                             profile; set_step_count: how many steps
 *   #7 step             u8    set_step: which step (its temperature is #3 setpoint_cc)
 *   #8 ramp_h           u16   set_step
 *   #9 hold_h           u16   set_step
 *
 * Settings (msg_type 4)
 *   #1 heat_diff_cc     u16   heating starts this far below the setpoint
 *   #2 cool_diff_cc     u16
 *   #3 heat_delay_s     u16   restart delays
 *   #4 cool_delay_s     u16
 *   #5 crash_default_cc i16   where Cold crash starts
 *   #6 profile          u8    0 constant, 1 schedule              (#6..#12 appended together;
 *   #7 step             u8    running step, 255 = finished          missing: constant, no steps)
 *   #8 flags            u8    bit0 the dial overrides the step's target
 *   #9 nsteps           u8    <= 8
 *   #10 step_temp_cc    i16 x nsteps
 *   #11 step_ramp_h     u16 x nsteps   hours to ramp there (0 = jump)
 *   #12 step_hold_h     u16 x nsteps   hours to hold it
 *
 * HistoryChunk (msg_type 3)
 *   #1 flags            u8    bit0 reset, bit1 more chunks follow
 *   #2 minutes_per_sample u8  rounded; 0 under 30 s (for decoders that predate #5)
 *   #3 n                u8    <= 24
 *   #4 temps_cc         i16 x n
 *   #5 seconds_per_sample u16 rounded, >= 1 (for decoders that predate #6; missing: #2 x 60)
 *   #6 ms_per_sample    u32   (missing: seconds_per_sample x 1000)
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
    void u32(uint32_t v)
    {
        u16(static_cast<uint16_t>(v & 0xFFFFu));
        u16(static_cast<uint16_t>(v >> 16));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
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
    uint32_t u32()
    {
        const uint32_t lo = u16();
        const uint32_t hi = u16();
        return lo | (hi << 16);
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
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
