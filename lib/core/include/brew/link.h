/**
 * Board-to-board transport pieces:
 *   - framing: COBS + CRC32 so a byte stream (UART, USB CDC, RS-485) carries whole packets
 *   - StreamPacketLink: hal::ByteStream -> hal::Link
 *   - RemoteControllerProxy: UiPort over a Link   (runs on the display board)
 *   - RemoteUiProxy:         ControlPort over a Link (runs on the controller board)
 *
 * The proxies only ever see bytes through brew::codec, never struct layouts.
 */
#pragma once

#include "brew/local_bus.h"
#include "brew/ports.h"
#include "brew/drivers/interfaces/io.h"
#include "brew/drivers/interfaces/platform.h"

#include <cstddef>
#include <cstdint>

namespace brew {

namespace framing {
uint32_t crc32(const uint8_t * data, size_t n);
/** COBS-encode `n` bytes (no delimiter added). Returns encoded length, 0 if `max` too small. */
size_t cobsEncode(const uint8_t * in, size_t n, uint8_t * out, size_t max);
/** Decode one COBS frame (without delimiter). Returns decoded length, 0 on malformed input. */
size_t cobsDecode(const uint8_t * in, size_t n, uint8_t * out, size_t max);
}  // namespace framing

/** Packets over a byte stream: [COBS(payload + crc32 LE)] 0x00 */
class StreamPacketLink final : public hal::Link {
public:
    explicit StreamPacketLink(hal::ByteStream & s) : s_(s) {}

    bool   begin() override { return s_.begin(); }
    bool   send(const uint8_t * data, size_t n) override;
    size_t recv(uint8_t * buf, size_t max) override;

    uint32_t droppedFrames() const { return dropped_; }

private:
    hal::ByteStream & s_;
    uint8_t frame_[hal::Link::kMaxPacket + 8];   // COBS bytes of the frame being received
    size_t flen_ = 0;
    bool overflow_ = false;
    uint8_t stage_[64];
    size_t spos_ = 0, slen_ = 0;
    uint32_t dropped_ = 0;
};

/**
 * Display side: talks to a controller on another board.
 *
 * Commands can be lost on the wire. Each Set* command stays pending until a State (or, for
 * SetTuning, Settings) shows it applied, and is re-sent every retry_ms meanwhile (they are idempotent, so duplicates are
 * harmless). While pending, its value is overlaid on received States, so the UI keeps showing
 * what the user chose instead of snapping back to the controller's old value. After give_up_ms
 * the command is dropped and the controller's value shows through.
 */
class RemoteControllerProxy final : public UiPort {
public:
    explicit RemoteControllerProxy(hal::Link & link, uint32_t resync_ms = 3000, uint32_t retry_ms = 500,
                                   uint32_t give_up_ms = 15000)
        : link_(link), resync_ms_(resync_ms), retry_ms_(retry_ms), give_up_ms_(give_up_ms) {}

    void send(const Command & cmd) override;
    bool pollState(State & out) override;
    bool pollHistory(HistoryChunk & out) override;
    bool pollSettings(Settings & out) override;
    /** Pumps the link, re-sends unconfirmed commands, and asks for a full sync at start and
     *  whenever State stops arriving. */
    void service(uint32_t now_ms) override;

    /** Commands sent but not yet confirmed by a State (for tests / diagnostics). */
    int pendingCommands() const;

private:
    /* One slot per independent setting: latest command wins within a slot. */
    enum Slot {
        kSpFerment, kSpCrash, kMode, kUnits, kTune0,
        kProfile = kTune0 + kTuningCount, kStepCount, kStep0,
        kSlots = kStep0 + Settings::kMaxSteps
    };
    struct Pending {
        bool active = false;
        Command cmd;
        uint32_t first_ms = 0, last_ms = 0;
    };

    void pump();
    void transmit(const Command & cmd);
    static int slotOf(const Command & c);
    static bool confirmed(const Command & c, const State & s);
    static void overlay(const Command & c, State & s);
    static bool confirmed(const Command & c, const Settings & s);
    static void overlay(const Command & c, Settings & s);

    hal::Link & link_;
    uint32_t resync_ms_, retry_ms_, give_up_ms_;
    uint16_t seq_ = 0;
    uint32_t now_ = 0, last_rx_ = 0, last_req_ = 0;
    bool requested_ = false;
    State state_;
    bool fresh_ = false;
    Settings settings_;
    bool settings_fresh_ = false;
    Pending pending_[kSlots];
    SpscRing<HistoryChunk, 32> hist_;   // chunks received but not yet polled
};

/** Controller side: talks to a display on another board. */
class RemoteUiProxy final : public ControlPort {
public:
    explicit RemoteUiProxy(hal::Link & link) : link_(link) {}

    bool pollCommand(Command & out) override;
    void publish(const State & s) override;
    bool publish(const HistoryChunk & h) override;
    void publish(const Settings & s) override;

private:
    void pump();

    hal::Link & link_;
    uint16_t seq_ = 0;
    SpscRing<Command, 16> cmds_;
};

}  // namespace brew
