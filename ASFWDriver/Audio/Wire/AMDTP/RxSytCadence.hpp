#pragma once

#include "AmdtpTiming.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Driver {

// Single-writer (IR), single-reader (TX pump) copy of the clock-recovery state
// used by Saffire.kext. ReadFirewireBuffers at 0xd69e-0xd81e records 512 SYT
// deltas and does not establish the device clock until the update after that
// history has filled. FillFirewireBuffers consumes the same ring 256 entries
// behind the RX writer (0xec5d-0xed72).
class RxSytCadence final {
public:
    static constexpr uint16_t kNoInfo = 0xFFFF;
    static constexpr uint32_t kEntryCount = 512;
    static constexpr uint32_t kReadDelay = kEntryCount / 2;
    static constexpr uint32_t kWarmupUpdates = kEntryCount + 1;

    struct Snapshot final {
        uint32_t epoch{0};
        uint16_t writeIndex{0};
        uint32_t validUpdates{0};
        uint32_t rollingCadenceTicks{0};
        int64_t recoveredPhaseTicks{0};
        bool established{false};
    };

    void Reset() noexcept {
        BeginWrite();
        for (auto& entry : entries_) {
            entry.store(0, std::memory_order_relaxed);
        }
        writeIndex_.store(256, std::memory_order_relaxed);
        agingIndex_.store(0, std::memory_order_relaxed);
        validUpdates_.store(0, std::memory_order_relaxed);
        rollingCadenceTicks_.store(0, std::memory_order_relaxed);
        previousSyt_.store(kNoInfo, std::memory_order_relaxed);
        recoveredPhaseTicks_.store(0, std::memory_order_relaxed);
        established_.store(false, std::memory_order_relaxed);
        epoch_.fetch_add(1, std::memory_order_relaxed);
        EndWrite();
    }

    // `packetCycleTimer` is the cycle-timer estimate for the packet carrying
    // this SYT, not the later drain/callback instant.
    bool Observe(uint16_t syt, uint32_t packetCycleTimer) noexcept {
        if (syt == kNoInfo) {
            return false;
        }

        BeginWrite();

        const uint16_t previous = previousSyt_.load(std::memory_order_relaxed);
        if (previous == kNoInfo) {
            // First SYT of a chain (start, or restart after an invalid delta)
            // only seeds previousSyt_. Recording a synthetic nominal delta
            // would bias the ring at any rate whose real step differs from
            // the seed constant (48k = 4096 ticks vs 44.1k = 4458/4459).
            previousSyt_.store(syt, std::memory_order_relaxed);
            EndWrite();
            return true;
        }
        const int64_t delta = ASFW::Timing::SYTDiffInOffsets(syt, previous);
        if (delta <= 0 || delta > UINT16_MAX) {
            previousSyt_.store(kNoInfo, std::memory_order_relaxed);
            EndWrite();
            return false;
        }

        const uint16_t writeIndex = writeIndex_.load(std::memory_order_relaxed);
        const uint16_t agingIndex = agingIndex_.load(std::memory_order_relaxed);
        const uint16_t outgoing = entries_[agingIndex].load(std::memory_order_relaxed);
        const uint16_t incoming = static_cast<uint16_t>(delta);

        entries_[writeIndex].store(incoming, std::memory_order_relaxed);
        writeIndex_.store(static_cast<uint16_t>((writeIndex + 1) & (kEntryCount - 1)),
                          std::memory_order_relaxed);
        agingIndex_.store(static_cast<uint16_t>((agingIndex + 1) & (kEntryCount - 1)),
                          std::memory_order_relaxed);

        const uint32_t rolling =
            rollingCadenceTicks_.load(std::memory_order_relaxed) - outgoing + incoming;
        rollingCadenceTicks_.store(rolling, std::memory_order_relaxed);
        previousSyt_.store(syt, std::memory_order_relaxed);

        const uint32_t updates =
            validUpdates_.load(std::memory_order_relaxed) + 1;
        validUpdates_.store(updates, std::memory_order_relaxed);
        established_.store(updates >= kWarmupUpdates, std::memory_order_relaxed);

        int64_t recovered =
            ASFW::Timing::extendTstampFromCycleTimer(packetCycleTimer, syt);
        recovered = ASFW::Timing::normalizeOffsetDomain(recovered);
        recoveredPhaseTicks_.store(recovered, std::memory_order_relaxed);

        EndWrite();
        return true;
    }

    [[nodiscard]] bool TrySnapshot(Snapshot& out,
                                   uint32_t maxAttempts = 4) const noexcept {
        for (uint32_t attempt = 0; attempt < maxAttempts; ++attempt) {
            const uint32_t before = sequence_.load(std::memory_order_acquire);
            if (before & 1u) {
                continue;
            }

            Snapshot snapshot{};
            snapshot.epoch = epoch_.load(std::memory_order_relaxed);
            snapshot.writeIndex = writeIndex_.load(std::memory_order_relaxed);
            snapshot.validUpdates = validUpdates_.load(std::memory_order_relaxed);
            snapshot.rollingCadenceTicks =
                rollingCadenceTicks_.load(std::memory_order_relaxed);
            snapshot.recoveredPhaseTicks =
                recoveredPhaseTicks_.load(std::memory_order_relaxed);
            snapshot.established = established_.load(std::memory_order_relaxed);

            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == before) {
                out = snapshot;
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] uint16_t ReadEntry(uint16_t index) const noexcept {
        return entries_[index & (kEntryCount - 1)].load(std::memory_order_acquire);
    }

private:
    void BeginWrite() noexcept {
        sequence_.fetch_add(1, std::memory_order_acq_rel);
    }

    void EndWrite() noexcept {
        sequence_.fetch_add(1, std::memory_order_release);
    }

    std::atomic<uint32_t> sequence_{0};
    std::atomic<uint32_t> epoch_{0};
    std::array<std::atomic<uint16_t>, kEntryCount> entries_{};
    std::atomic<uint16_t> writeIndex_{256};
    std::atomic<uint16_t> agingIndex_{0};
    std::atomic<uint32_t> validUpdates_{0};
    std::atomic<uint32_t> rollingCadenceTicks_{0};
    std::atomic<uint16_t> previousSyt_{kNoInfo};
    std::atomic<int64_t> recoveredPhaseTicks_{0};
    std::atomic<bool> established_{false};
};

static_assert((RxSytCadence::kEntryCount &
               (RxSytCadence::kEntryCount - 1)) == 0);
static_assert(std::atomic<int64_t>::is_always_lock_free);

} // namespace ASFW::Driver
