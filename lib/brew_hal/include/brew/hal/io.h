/**
 * Digital I/O and buses. GPIO expanders (PCF8574, MCP23008, ...) hand out DigitalIn /
 * DigitalOut objects for their pins, so a relay or button can move between a native GPIO
 * and an expander by changing one line in a board file.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace brew::hal {

class DigitalIn {
public:
    virtual ~DigitalIn() = default;
    virtual bool read() = 0;   // electrical level: true = high
};

class DigitalOut {
public:
    virtual ~DigitalOut() = default;
    virtual void write(bool high) = 0;
};

/**
 * Shared I2C bus. Single transfers are atomic. Use lock()/unlock() (or I2cLock) around
 * multi-transfer sequences that must not interleave with other tasks.
 */
class I2cBus {
public:
    virtual ~I2cBus() = default;
    virtual bool write(uint8_t addr, const uint8_t * data, size_t n) = 0;
    virtual bool writeRead(uint8_t addr, const uint8_t * w, size_t wn, uint8_t * r, size_t rn) = 0;
    virtual bool read(uint8_t addr, uint8_t * r, size_t rn) = 0;
    virtual bool probe(uint8_t addr) = 0;
    virtual void lock() = 0;
    virtual void unlock() = 0;
};

class I2cLock {
public:
    explicit I2cLock(I2cBus & b) : bus_(b) { bus_.lock(); }
    ~I2cLock() { bus_.unlock(); }
    I2cLock(const I2cLock &) = delete;
    I2cLock & operator=(const I2cLock &) = delete;

private:
    I2cBus & bus_;
};

/** Byte stream (UART, USB CDC, RS-485). Non-blocking. */
class ByteStream {
public:
    virtual ~ByteStream() = default;
    virtual bool   begin() = 0;
    virtual size_t write(const uint8_t * data, size_t n) = 0;   // bytes accepted
    virtual size_t read(uint8_t * buf, size_t max) = 0;         // bytes available now, 0 = none
};

}  // namespace brew::hal
