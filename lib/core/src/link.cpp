#include "brew/link.h"

#include "brew/codec.h"

namespace brew {

/* -------------------------------- framing -------------------------------- */

namespace framing {

uint32_t crc32(const uint8_t * p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for(size_t i = 0; i < n; i++) {
        c ^= p[i];
        for(int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

size_t cobsEncode(const uint8_t * in, size_t n, uint8_t * out, size_t max)
{
    if(max < n + n / 254 + 1) return 0;
    size_t code_at = 0, o = 1;
    uint8_t code = 1;
    for(size_t i = 0; i < n; i++) {
        if(in[i] == 0) {
            out[code_at] = code;
            code_at = o++;
            code = 1;
        }
        else {
            out[o++] = in[i];
            if(++code == 0xFF) {
                out[code_at] = code;
                code_at = o++;
                code = 1;
            }
        }
    }
    out[code_at] = code;
    return o;
}

size_t cobsDecode(const uint8_t * in, size_t n, uint8_t * out, size_t max)
{
    size_t i = 0, o = 0;
    while(i < n) {
        const uint8_t code = in[i++];
        if(code == 0) return 0;
        for(uint8_t k = 1; k < code; k++) {
            if(i >= n || in[i] == 0 || o >= max) return 0;
            out[o++] = in[i++];
        }
        if(code != 0xFF && i < n) {
            if(o >= max) return 0;
            out[o++] = 0;
        }
    }
    return o;
}

}  // namespace framing

/* ---------------------------- StreamPacketLink ---------------------------- */

bool StreamPacketLink::send(const uint8_t * data, size_t n)
{
    if(n > hal::Link::kMaxPacket) return false;
    uint8_t raw[hal::Link::kMaxPacket + 4];
    for(size_t i = 0; i < n; i++) raw[i] = data[i];
    const uint32_t crc = framing::crc32(data, n);
    for(int k = 0; k < 4; k++) raw[n + k] = static_cast<uint8_t>(crc >> (8 * k));

    uint8_t enc[sizeof(raw) + sizeof(raw) / 254 + 3];
    size_t m = framing::cobsEncode(raw, n + 4, enc, sizeof(enc) - 1);
    if(m == 0) return false;
    enc[m++] = 0;
    return s_.write(enc, m) == m;
}

size_t StreamPacketLink::recv(uint8_t * buf, size_t max)
{
    for(;;) {
        if(spos_ == slen_) {
            slen_ = s_.read(stage_, sizeof(stage_));
            spos_ = 0;
            if(slen_ == 0) return 0;
        }
        const uint8_t b = stage_[spos_++];
        if(b != 0) {
            if(flen_ < sizeof(frame_)) frame_[flen_++] = b;
            else overflow_ = true;
            continue;
        }
        /* delimiter: one complete frame */
        const size_t flen = flen_;
        const bool overflow = overflow_;
        flen_ = 0;
        overflow_ = false;
        if(flen == 0) continue;   // idle delimiters
        uint8_t raw[sizeof(frame_)];
        const size_t n = overflow ? 0 : framing::cobsDecode(frame_, flen, raw, sizeof(raw));
        if(n < 5) { dropped_++; continue; }
        const uint32_t want = static_cast<uint32_t>(raw[n - 4]) | (static_cast<uint32_t>(raw[n - 3]) << 8) |
                              (static_cast<uint32_t>(raw[n - 2]) << 16) | (static_cast<uint32_t>(raw[n - 1]) << 24);
        if(framing::crc32(raw, n - 4) != want || n - 4 > max) { dropped_++; continue; }
        for(size_t i = 0; i < n - 4; i++) buf[i] = raw[i];
        return n - 4;
    }
}

/* -------------------------- RemoteControllerProxy -------------------------- */

namespace {
constexpr float kSetpointTolC = 0.01f;   // the wire carries centi-degC
}

int RemoteControllerProxy::slotOf(const Command & c)
{
    switch(c.kind) {
        case Command::Kind::SetSetpoint: return c.mode == Mode::Crash ? kSpCrash : kSpFerment;
        case Command::Kind::SetMode:     return kMode;
        case Command::Kind::SetUnits:    return kUnits;
        case Command::Kind::SetTuning:   return kTune0 + static_cast<int>(c.tuning);
        default:                         return -1;   // RequestSync: answered by State itself
    }
}

bool RemoteControllerProxy::confirmed(const Command & c, const State & s)
{
    switch(c.kind) {
        case Command::Kind::SetSetpoint: {
            const float d = s.setpoint_c[c.mode == Mode::Crash ? 1 : 0] - c.setpoint_c;
            return d < kSetpointTolC && d > -kSetpointTolC;
        }
        case Command::Kind::SetMode:  return s.mode == c.mode && s.paused == c.paused;
        case Command::Kind::SetUnits: return s.celsius == c.celsius;
        default:                      return true;
    }
}

void RemoteControllerProxy::overlay(const Command & c, State & s)
{
    switch(c.kind) {
        case Command::Kind::SetSetpoint: s.setpoint_c[c.mode == Mode::Crash ? 1 : 0] = c.setpoint_c; break;
        case Command::Kind::SetMode:     s.mode = c.mode; s.paused = c.paused; break;
        case Command::Kind::SetUnits:    s.celsius = c.celsius; break;
        default: break;
    }
}

/* SetTuning: confirmed / shown by Settings (values in centi-C or s, as on the wire) */
bool RemoteControllerProxy::confirmed(const Command & c, const Settings & s)
{
    if(c.kind != Command::Kind::SetTuning) return false;
    auto cc = [](float v) { return static_cast<int32_t>(v * 100.0f + (v < 0 ? -0.5f : 0.5f)); };
    switch(c.tuning) {
        case Tuning::HeatDiff:     return cc(s.heat_diff_c) == c.value;
        case Tuning::CoolDiff:     return cc(s.cool_diff_c) == c.value;
        case Tuning::HeatDelay:    return s.heat_delay_s == c.value;
        case Tuning::CoolDelay:    return s.cool_delay_s == c.value;
        case Tuning::CrashDefault: return cc(s.crash_default_c) == c.value;
    }
    return true;
}

void RemoteControllerProxy::overlay(const Command & c, Settings & s)
{
    if(c.kind != Command::Kind::SetTuning) return;
    switch(c.tuning) {
        case Tuning::HeatDiff:     s.heat_diff_c = static_cast<float>(c.value) / 100.0f; break;
        case Tuning::CoolDiff:     s.cool_diff_c = static_cast<float>(c.value) / 100.0f; break;
        case Tuning::HeatDelay:    s.heat_delay_s = static_cast<uint16_t>(c.value); break;
        case Tuning::CoolDelay:    s.cool_delay_s = static_cast<uint16_t>(c.value); break;
        case Tuning::CrashDefault: s.crash_default_c = static_cast<float>(c.value) / 100.0f; break;
    }
}

void RemoteControllerProxy::transmit(const Command & cmd)
{
    uint8_t b[codec::kMaxEncoded];
    const size_t n = codec::encode(cmd, seq_++, b, sizeof(b));
    if(n) link_.send(b, n);
}

void RemoteControllerProxy::send(const Command & cmd)
{
    const int k = slotOf(cmd);
    if(k >= 0) {
        Pending & p = pending_[k];
        p.active = true;
        p.cmd = cmd;
        p.first_ms = p.last_ms = now_;
    }
    transmit(cmd);
}

int RemoteControllerProxy::pendingCommands() const
{
    int n = 0;
    for(const Pending & p : pending_) n += p.active ? 1 : 0;
    return n;
}

void RemoteControllerProxy::pump()
{
    uint8_t b[hal::Link::kMaxPacket];
    size_t n;
    codec::Message m;
    while((n = link_.recv(b, sizeof(b))) > 0) {
        if(codec::decode(b, n, m) != codec::DecodeResult::Ok) continue;
        if(m.type == codec::MsgType::State) {
            state_ = m.state;
            fresh_ = true;
            last_rx_ = now_;
            for(Pending & p : pending_)
                if(p.active && p.cmd.kind != Command::Kind::SetTuning && confirmed(p.cmd, state_)) p.active = false;
        }
        else if(m.type == codec::MsgType::Settings) {
            settings_ = m.settings;
            settings_fresh_ = true;
            for(Pending & p : pending_)
                if(p.active && p.cmd.kind == Command::Kind::SetTuning && confirmed(p.cmd, settings_)) p.active = false;
        }
        else if(m.type == codec::MsgType::History) {
            hist_.push(m.history);
        }
    }
}

bool RemoteControllerProxy::pollState(State & out)
{
    pump();
    if(!fresh_) return false;
    fresh_ = false;
    out = state_;
    for(const Pending & p : pending_)
        if(p.active) overlay(p.cmd, out);
    return true;
}

bool RemoteControllerProxy::pollSettings(Settings & out)
{
    pump();
    if(!settings_fresh_) return false;
    settings_fresh_ = false;
    out = settings_;
    for(const Pending & p : pending_)
        if(p.active) overlay(p.cmd, out);
    return true;
}

bool RemoteControllerProxy::pollHistory(HistoryChunk & out)
{
    pump();
    return hist_.pop(out);
}

void RemoteControllerProxy::service(uint32_t now)
{
    now_ = now;
    pump();
    for(Pending & p : pending_) {
        if(!p.active) continue;
        if(now - p.first_ms >= give_up_ms_) {
            p.active = false;   // controller refused it or is gone: show its real value
            fresh_ = true;
            settings_fresh_ = true;
        }
        else if(now - p.last_ms >= retry_ms_) {
            p.last_ms = now;
            transmit(p.cmd);
        }
    }
    const bool stale = !requested_ || now - last_rx_ > resync_ms_;
    if(stale && (!requested_ || now - last_req_ >= resync_ms_)) {
        send(Command::requestSync());
        requested_ = true;
        last_req_ = now;
        if(last_rx_ == 0) last_rx_ = now;   // give the first answer resync_ms to arrive
    }
}

/* ------------------------------ RemoteUiProxy ------------------------------ */

void RemoteUiProxy::pump()
{
    uint8_t b[hal::Link::kMaxPacket];
    size_t n;
    codec::Message m;
    while((n = link_.recv(b, sizeof(b))) > 0) {
        if(codec::decode(b, n, m) == codec::DecodeResult::Ok && m.type == codec::MsgType::Command)
            cmds_.push(m.command);
    }
}

bool RemoteUiProxy::pollCommand(Command & out)
{
    pump();
    return cmds_.pop(out);
}

void RemoteUiProxy::publish(const State & s)
{
    uint8_t b[codec::kMaxEncoded];
    const size_t n = codec::encode(s, seq_++, b, sizeof(b));
    if(n) link_.send(b, n);
}

void RemoteUiProxy::publish(const Settings & s)
{
    uint8_t b[codec::kMaxEncoded];
    const size_t n = codec::encode(s, seq_++, b, sizeof(b));
    if(n) link_.send(b, n);
}

bool RemoteUiProxy::publish(const HistoryChunk & h)
{
    uint8_t b[codec::kMaxEncoded];
    const size_t n = codec::encode(h, seq_++, b, sizeof(b));
    return n && link_.send(b, n);
}

}  // namespace brew
