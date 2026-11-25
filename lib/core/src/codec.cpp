#include "brew/codec.h"

#include "wire_v1.h"

#include <cstring>

namespace brew::codec {

using v1::from_cc;
using v1::Reader;
using v1::to_cc;
using v1::Writer;

namespace {

void header(Writer & w, MsgType t, uint16_t seq)
{
    w.u8(kProtoVersion);
    w.u8(static_cast<uint8_t>(t));
    w.u16(seq);
}

}  // namespace

size_t encode(const State & s, uint16_t seq, uint8_t * buf, size_t max)
{
    Writer w(buf, max);
    header(w, MsgType::State, seq);
    w.i16(to_cc(s.temp_c));                                         // #1
    w.i16(to_cc(s.setpoint_c[0]));                                  // #2
    w.i16(to_cc(s.setpoint_c[1]));                                  // #3
    w.u8(static_cast<uint8_t>(s.mode));                             // #4
    w.u8(static_cast<uint8_t>(s.action));                           // #5
    w.u8(static_cast<uint8_t>((s.paused ? 1u : 0u) | (s.celsius ? 2u : 0u) |
                              (s.temp_valid ? 4u : 0u)));           // #6
    w.u16(s.day);                                                   // #7
    size_t len = strnlen(s.batch, State::kBatchLen - 1);
    w.u8(static_cast<uint8_t>(len));                                // #8
    w.bytes(reinterpret_cast<const uint8_t *>(s.batch), len);
    w.u8(static_cast<uint8_t>(s.wanted));                           // #9
    w.u16(s.wait_s);                                                // #10
    return w.size();
}

size_t encode(const Command & c, uint16_t seq, uint8_t * buf, size_t max)
{
    Writer w(buf, max);
    header(w, MsgType::Command, seq);
    w.u8(static_cast<uint8_t>(c.kind));                             // #1
    w.u8(static_cast<uint8_t>(c.mode));                             // #2
    w.i16(to_cc(c.setpoint_c));                                     // #3
    w.u8(static_cast<uint8_t>((c.paused ? 1u : 0u) | (c.celsius ? 2u : 0u)));   // #4
    w.u8(static_cast<uint8_t>(c.tuning));                           // #5
    w.i32(c.value);                                                 // #6
    w.u8(c.step);                                                   // #7
    w.u16(c.ramp_h);                                                // #8
    w.u16(c.hold_h);                                                // #9
    const size_t nl = strnlen(c.name, State::kBatchLen - 1);        // #10
    w.u8(static_cast<uint8_t>(nl));
    w.bytes(reinterpret_cast<const uint8_t *>(c.name), nl);
    return w.size();
}

size_t encode(const Settings & s, uint16_t seq, uint8_t * buf, size_t max)
{
    Writer w(buf, max);
    header(w, MsgType::Settings, seq);
    w.u16(static_cast<uint16_t>(to_cc(s.heat_diff_c)));             // #1
    w.u16(static_cast<uint16_t>(to_cc(s.cool_diff_c)));             // #2
    w.u16(s.heat_delay_s);                                          // #3
    w.u16(s.cool_delay_s);                                          // #4
    w.i16(to_cc(s.crash_default_c));                                // #5
    const uint8_t n = s.nsteps > Settings::kMaxSteps ? Settings::kMaxSteps : s.nsteps;
    w.u8(static_cast<uint8_t>(s.profile));                          // #6
    w.u8(s.step);                                                   // #7
    w.u8(static_cast<uint8_t>((s.override_on ? 1u : 0u) | (s.simulated ? 2u : 0u)));   // #8
    w.u8(n);                                                        // #9
    for(uint8_t i = 0; i < n; i++) w.i16(to_cc(s.steps[i].temp_c)); // #10
    for(uint8_t i = 0; i < n; i++) w.u16(s.steps[i].ramp_h);        // #11
    for(uint8_t i = 0; i < n; i++) w.u16(s.steps[i].hold_h);        // #12
    return w.size();
}

size_t encode(const HistoryChunk & h, uint16_t seq, uint8_t * buf, size_t max)
{
    Writer w(buf, max);
    header(w, MsgType::History, seq);
    const uint8_t n = h.n > HistoryChunk::kMax ? HistoryChunk::kMax : h.n;
    w.u8(static_cast<uint8_t>((h.reset ? 1u : 0u) | (h.more ? 2u : 0u)));   // #1
    const uint32_t ms = h.ms_per_sample;
    const uint32_t min = (ms + 30000u) / 60000u;                    // whole minutes; 0 under 30 s
    w.u8(static_cast<uint8_t>(min > 255u ? 255u : min));            // #2
    w.u8(n);                                                        // #3
    for(uint8_t i = 0; i < n; i++) w.i16(to_cc(h.temps_c[i]));      // #4
    uint32_t sec = (ms + 500u) / 1000u;                             // whole seconds, >= 1
    if(sec == 0 && ms > 0) sec = 1;
    w.u16(static_cast<uint16_t>(sec > 0xFFFFu ? 0xFFFFu : sec));    // #5
    w.u16(static_cast<uint16_t>(ms & 0xFFFFu));                     // #6 ms, low half
    w.u16(static_cast<uint16_t>(ms >> 16));                         //    ...high half (u32 LE)
    return w.size();
}

namespace {

DecodeResult decodeState(Reader & r, State & s)
{
    if(!r.has(9)) return DecodeResult::BadPayload;   // #1..#6 are mandatory
    s = State{};
    s.temp_c = from_cc(r.i16());
    s.setpoint_c[0] = from_cc(r.i16());
    s.setpoint_c[1] = from_cc(r.i16());
    const uint8_t mode = r.u8();
    const uint8_t action = r.u8();
    if(mode >= kModeCount || action > static_cast<uint8_t>(Action::Heating)) return DecodeResult::BadPayload;
    s.mode = static_cast<Mode>(mode);
    s.action = static_cast<Action>(action);
    const uint8_t f = r.u8();
    s.paused = f & 1u;
    s.celsius = f & 2u;
    s.temp_valid = f & 4u;
    s.day = r.u16();
    uint8_t len = r.u8();
    if(len > State::kBatchLen - 1) return DecodeResult::BadPayload;
    for(uint8_t i = 0; i < len; i++) {
        if(!r.has(1)) return DecodeResult::BadPayload;
        s.batch[i] = static_cast<char>(r.u8());
    }
    s.batch[len] = '\0';
    /* appended later: an older controller omits them, and then is never "waiting" */
    s.wanted = s.action;
    if(r.has(1)) {
        const uint8_t wanted = r.u8();
        if(wanted > static_cast<uint8_t>(Action::Heating)) return DecodeResult::BadPayload;
        s.wanted = static_cast<Action>(wanted);
    }
    s.wait_s = r.u16();   // 0 if missing
    return DecodeResult::Ok;
}

DecodeResult decodeCommand(Reader & r, Command & c)
{
    if(!r.has(5)) return DecodeResult::BadPayload;
    c = Command{};
    const uint8_t kind = r.u8();
    if(kind < static_cast<uint8_t>(Command::Kind::SetSetpoint) ||
       kind > static_cast<uint8_t>(Command::Kind::SetBatch))
        return DecodeResult::BadPayload;
    c.kind = static_cast<Command::Kind>(kind);
    const uint8_t mode = r.u8();
    if(mode >= kModeCount) return DecodeResult::BadPayload;
    c.mode = static_cast<Mode>(mode);
    c.setpoint_c = from_cc(r.i16());
    const uint8_t f = r.u8();
    c.paused = f & 1u;
    c.celsius = f & 2u;
    if(r.has(5)) {   // #5, #6 (appended)
        const uint8_t t = r.u8();
        c.value = r.i32();
        if(t >= kTuningCount) {
            if(c.kind == Command::Kind::SetTuning) return DecodeResult::BadPayload;
        }
        else {
            c.tuning = static_cast<Tuning>(t);
        }
    }
    else if(c.kind == Command::Kind::SetTuning || c.kind == Command::Kind::SetProfile ||
            c.kind == Command::Kind::SetStepCount || c.kind == Command::Kind::SetClock || c.kind == Command::Kind::SetBatch) {
        return DecodeResult::BadPayload;
    }
    if(r.has(5)) {   // #7..#9 (appended)
        c.step = r.u8();
        c.ramp_h = r.u16();
        c.hold_h = r.u16();
    }
    else if(c.kind == Command::Kind::SetStep || c.kind == Command::Kind::SetBatch) {
        return DecodeResult::BadPayload;
    }
    if(r.has(1)) {   // #10 (appended)
        const uint8_t len = r.u8();
        if(len > State::kBatchLen - 1 || !r.has(len)) return DecodeResult::BadPayload;
        for(uint8_t i = 0; i < len; i++) c.name[i] = static_cast<char>(r.u8());
        c.name[len] = '\0';
    }
    else if(c.kind == Command::Kind::SetBatch) {
        return DecodeResult::BadPayload;
    }
    return DecodeResult::Ok;
}

DecodeResult decodeSettings(Reader & r, Settings & s)
{
    if(!r.has(10)) return DecodeResult::BadPayload;
    s = Settings{};
    s.heat_diff_c = from_cc(static_cast<int16_t>(r.u16()));
    s.cool_diff_c = from_cc(static_cast<int16_t>(r.u16()));
    s.heat_delay_s = r.u16();
    s.cool_delay_s = r.u16();
    s.crash_default_c = from_cc(r.i16());
    if(r.has(4)) {   // #6..#12 (appended)
        const uint8_t p = r.u8();
        if(p >= kProfileCount) return DecodeResult::BadPayload;
        s.profile = static_cast<Profile>(p);
        s.step = r.u8();
        const uint8_t fl = r.u8();
        s.override_on = fl & 1u;
        s.simulated = fl & 2u;
        const uint8_t n = r.u8();
        if(n > Settings::kMaxSteps || !r.has(static_cast<size_t>(n) * 6)) return DecodeResult::BadPayload;
        s.nsteps = n;
        for(uint8_t i = 0; i < n; i++) s.steps[i].temp_c = from_cc(r.i16());
        for(uint8_t i = 0; i < n; i++) s.steps[i].ramp_h = r.u16();
        for(uint8_t i = 0; i < n; i++) s.steps[i].hold_h = r.u16();
    }
    return DecodeResult::Ok;
}

DecodeResult decodeHistory(Reader & r, HistoryChunk & h)
{
    if(!r.has(3)) return DecodeResult::BadPayload;
    h = HistoryChunk{};
    const uint8_t f = r.u8();
    h.reset = f & 1u;
    h.more = f & 2u;
    const uint8_t minutes = r.u8();
    h.n = r.u8();
    if(h.n > HistoryChunk::kMax || !r.has(static_cast<size_t>(h.n) * 2)) return DecodeResult::BadPayload;
    for(uint8_t i = 0; i < h.n; i++) h.temps_c[i] = from_cc(r.i16());
    /* #5 and #6 appended later: an older controller sends whole minutes, then whole seconds */
    h.ms_per_sample = static_cast<uint32_t>(minutes) * 60000u;
    if(r.has(2)) h.ms_per_sample = static_cast<uint32_t>(r.u16()) * 1000u;
    if(r.has(4)) {
        const uint32_t lo = r.u16();
        h.ms_per_sample = lo | (static_cast<uint32_t>(r.u16()) << 16);
    }
    return DecodeResult::Ok;
}

}  // namespace

DecodeResult decode(const uint8_t * buf, size_t n, Message & out)
{
    if(n < kHeaderSize) return DecodeResult::TooShort;
    Reader r(buf, n);
    if(r.u8() != kProtoVersion) return DecodeResult::BadVersion;
    const uint8_t type = r.u8();
    out.seq = r.u16();
    switch(type) {
        case static_cast<uint8_t>(MsgType::State):
            out.type = MsgType::State;
            return decodeState(r, out.state);
        case static_cast<uint8_t>(MsgType::Command):
            out.type = MsgType::Command;
            return decodeCommand(r, out.command);
        case static_cast<uint8_t>(MsgType::Settings):
            out.type = MsgType::Settings;
            return decodeSettings(r, out.settings);
        case static_cast<uint8_t>(MsgType::History):
            out.type = MsgType::History;
            return decodeHistory(r, out.history);
        default:
            return DecodeResult::UnknownType;
    }
}

}  // namespace brew::codec
