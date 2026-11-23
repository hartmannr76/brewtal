/**
 * The ONLY place domain types become bytes (and back). See CLAUDE.md "Wire format rules".
 *
 * Every encoded message starts with a fixed header:
 *     [proto_ver:u8][msg_type:u8][seq:u16 LE][payload...]
 * The payload layout for each version lives in src/wire_v1.h (private to lib/core).
 * Moving to nanopb later = new proto_ver + a new codec implementation; nothing else changes.
 */
#pragma once

#include "brew/types.h"

#include <cstddef>
#include <cstdint>

namespace brew::codec {

constexpr uint8_t kProtoVersion = 1;
constexpr size_t kHeaderSize = 4;
constexpr size_t kMaxEncoded = 128;   // fits every v1 message, below hal::Link::kMaxPacket

enum class MsgType : uint8_t {
    State = 1,
    Command = 2,
    History = 3,
};

struct Message {
    MsgType      type = MsgType::State;
    uint16_t     seq = 0;
    State        state;
    Command      command;
    HistoryChunk history;
};

enum class DecodeResult : uint8_t { Ok, TooShort, BadVersion, UnknownType, BadPayload };

/** Return the encoded size, or 0 if `max` is too small. */
size_t encode(const State & s, uint16_t seq, uint8_t * buf, size_t max);
size_t encode(const Command & c, uint16_t seq, uint8_t * buf, size_t max);
size_t encode(const HistoryChunk & h, uint16_t seq, uint8_t * buf, size_t max);

/** Decodes one message. Trailing bytes after the known fields are ignored (append-only evolution). */
DecodeResult decode(const uint8_t * buf, size_t n, Message & out);

}  // namespace brew::codec
