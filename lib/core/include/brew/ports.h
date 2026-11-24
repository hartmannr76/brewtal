/**
 * The only way the UI side and the controller side talk to each other.
 *
 *   UI side  --Command-->                controller
 *   UI side  <--State/History/Settings-- controller
 *
 * Same board: LocalBus (lock-free rings between two tasks).
 * Separate boards: RemoteControllerProxy / RemoteUiProxy over a hal::Link.
 * App code cannot tell which one it has.
 */
#pragma once

#include "brew/types.h"

namespace brew {

/** What the UI side sees of the controller. Call from one task only. */
class UiPort {
public:
    virtual ~UiPort() = default;
    virtual void send(const Command & cmd) = 0;
    /** True when a State newer than the last one returned is available. */
    virtual bool pollState(State & out) = 0;
    virtual bool pollHistory(HistoryChunk & out) = 0;
    /** True when Settings newer than the last ones returned are available. */
    virtual bool pollSettings(Settings & out) { (void)out; return false; }
    /** Housekeeping (link pumping, reconnect). Call every loop. */
    virtual void service(uint32_t now_ms) { (void)now_ms; }
};

/** What the controller sees of the UI. Call from one task only. */
class ControlPort {
public:
    virtual ~ControlPort() = default;
    virtual bool pollCommand(Command & out) = 0;
    virtual void publish(const State & s) = 0;
    /** False if it could not be queued right now (full): the controller tries again later. */
    virtual bool publish(const HistoryChunk & h) = 0;
    virtual void publish(const Settings & s) { (void)s; }
    virtual void service(uint32_t now_ms) { (void)now_ms; }
};

}  // namespace brew
