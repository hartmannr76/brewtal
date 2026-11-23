/**
 * Platform services: persistent key/value settings and packet links between boards.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace brew::hal {

/** Small persistent key/value store (NVS on ESP, a file or RAM on host). */
class SettingsStore {
public:
    virtual ~SettingsStore() = default;
    virtual bool getI32(const char * key, int32_t & out) = 0;   // false = not stored
    virtual bool setI32(const char * key, int32_t value) = 0;
    virtual bool getStr(const char * key, char * out, size_t max) = 0;
    virtual bool setStr(const char * key, const char * value) = 0;
    virtual bool commit() = 0;
};

/**
 * Packet link between two boards (display <-> controller). One send() = one packet on the
 * other side's recv(), or nothing (corrupt/lost packets are dropped, never half-delivered).
 * Stream transports get this via brew::StreamPacketLink (COBS + CRC32 framing).
 */
class Link {
public:
    static constexpr size_t kMaxPacket = 256;

    virtual ~Link() = default;
    virtual bool   begin() = 0;
    virtual bool   send(const uint8_t * data, size_t n) = 0;
    virtual size_t recv(uint8_t * buf, size_t max) = 0;   // non-blocking, 0 = nothing
};

}  // namespace brew::hal
