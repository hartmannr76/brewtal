/**
 * Same-board transport between the UI task and the control task. Lock-free, no RTOS calls,
 * so it works the same on ESP32 (two FreeRTOS tasks) and on host (one thread or two).
 *
 * Exactly one producer and one consumer per direction (that is what UiPort/ControlPort
 * promise), so single-producer/single-consumer structures are enough.
 */
#pragma once

#include "brew/ports.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace brew {

/** Bounded SPSC ring. N must be a power of two. push() fails (drops) when full. */
template<typename T, size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

public:
    bool push(const T & v)
    {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        if(h - tail_.load(std::memory_order_acquire) == N) return false;
        buf_[h & (N - 1)] = v;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pop(T & out)
    {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        if(t == head_.load(std::memory_order_acquire)) return false;
        out = buf_[t & (N - 1)];
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    /** Consumer side only. */
    bool empty() const
    {
        return tail_.load(std::memory_order_relaxed) == head_.load(std::memory_order_acquire);
    }

private:
    T buf_[N];
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
};

/**
 * Latest-value mailbox (seqlock). The writer never blocks; the reader retries if it raced
 * a write. Suits State: only the newest one matters.
 */
template<typename T>
class Mailbox {
    static_assert(std::is_trivially_copyable<T>::value, "Mailbox needs a trivially copyable T");

public:
    void write(const T & v)
    {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);   // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);
        std::memcpy(&val_, &v, sizeof(T));
        std::atomic_thread_fence(std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);   // even: stable
    }

    /** Copies the value if it changed since `seen`; updates `seen`. */
    bool readIfNew(T & out, uint32_t & seen) const
    {
        for(;;) {
            const uint32_t s1 = seq_.load(std::memory_order_acquire);
            if(s1 == seen || s1 == 0) return false;
            if(s1 & 1u) continue;
            T tmp;
            std::memcpy(&tmp, &val_, sizeof(T));
            std::atomic_thread_fence(std::memory_order_acquire);
            if(seq_.load(std::memory_order_relaxed) != s1) continue;
            out = tmp;
            seen = s1;
            return true;
        }
    }

private:
    T val_{};
    std::atomic<uint32_t> seq_{0};
};

class LocalBus final : public UiPort, public ControlPort {
public:
    // UiPort (UI task)
    void send(const Command & cmd) override { commands_.push(cmd); }
    bool pollState(State & out) override { return state_.readIfNew(out, seen_); }
    bool pollHistory(HistoryChunk & out) override { return history_.pop(out); }

    // ControlPort (control task)
    bool pollCommand(Command & out) override { return commands_.pop(out); }
    void publish(const State & s) override { state_.write(s); }
    void publish(const HistoryChunk & h) override { history_.push(h); }

    void service(uint32_t) override {}   // nothing to pump (overrides both bases)

private:
    SpscRing<Command, 16> commands_;
    SpscRing<HistoryChunk, 16> history_;   // a full sync is 5 chunks
    Mailbox<State> state_;
    uint32_t seen_ = 0;
};

}  // namespace brew
