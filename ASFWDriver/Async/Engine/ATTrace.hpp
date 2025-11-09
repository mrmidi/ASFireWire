#pragma once

#include <array>
#include <atomic>
#include <cstdint>
// #include <mach/mach_time.h> - doesn't belogs to user space!
#include <DriverKit/IOLib.h>

namespace ASFW::Async::Engine {

/**
 * ATEvent - AT state machine event types for black box tracing
 * Used in ATTraceRing to record last 256 events for panic debugging
 */
enum class ATEvent : uint8_t {
    P1_ARM,       ///< PATH 1: CommandPtr programmed + RUN set
    P2_LNK,       ///< PATH 2: Branch word linked
    P2_WAKE,      ///< PATH 2: WAKE bit set successfully
    P2_FALLBACK,  ///< PATH 2 failed, falling back to PATH 1
    STOP_IMM,     ///< Immediate stop (needsFlush=true)
    STOP_DRAIN,   ///< AR-side stop (outstanding==0)
    RESET,        ///< Context reset
    ERROR         ///< Fatal error state
};

/**
 * ATTrace - Single trace event entry
 * Captures timing, txid, generation, event type, and overloaded context (ctrl word, head/tail)
 */
struct ATTrace {
    uint64_t t_ns;     ///< Timestamp in nanoseconds (monotonic)
    uint32_t txid;     ///< Transaction ID for correlation
    uint16_t gen;      ///< Bus generation at time of event
    ATEvent ev;        ///< Event type
    uint32_t a;        ///< Overloaded: ctrl word, elapsed_us, cmdPtr, etc.
    uint32_t b;        ///< Overloaded: z field, head/tail index, etc.
};

/**
 * ATTraceRing - Lock-free ring buffer for AT event tracing
 * Stores last 256 events for panic debugging and post-mortem analysis.
 * 
 * Thread-safe: push() uses atomic fetch_add, no locks required.
 * Dump on ERROR state or panic to diagnose state transitions.
 */
class ATTraceRing {
public:
    ATTraceRing() : idx_(0) {}

    /**
     * Push event to ring buffer (lock-free, thread-safe)
     * @param e Event to record
     */
    void push(const ATTrace& e) noexcept {
        const uint32_t i = idx_.fetch_add(1, std::memory_order_relaxed) & 0xFF;
        buf_[i] = e;
    }

    /**
     * Dump last 256 events to logs (for panic/ERROR state analysis)
     * Call from panic handler or on ERROR state transition
     */
    void dump() const noexcept;

    /**
     * Get current write index (for diagnostics)
     */
    uint32_t GetIndex() const noexcept {
        return idx_.load(std::memory_order_relaxed);
    }

    /**
     * Clear all events (reset to empty state)
     */
    void clear() noexcept {
        idx_.store(0, std::memory_order_relaxed);
        // Note: We don't zero the buffer, just let old entries be overwritten
    }

private:
    std::array<ATTrace, 256> buf_{};           ///< Ring buffer storage
    std::atomic<uint32_t> idx_{0};             ///< Write index (wraps at 256)
};

/**
 * Get timestamp in nanoseconds (monotonic)
 * Helper for ATTrace construction
 */
inline uint64_t NowNs() {
    static mach_timebase_info_data_t tb{0, 0};
    if (!tb.denom) {
        mach_timebase_info(&tb);
    }
    const uint64_t t = mach_absolute_time();
    return (t * tb.numer) / tb.denom;
}

/**
 * Get timestamp in microseconds (monotonic)
 * Helper for elapsed time calculations
 */
inline uint64_t NowUs() {
    return NowNs() / 1000;
}

} // namespace ASFW::Async::Engine
