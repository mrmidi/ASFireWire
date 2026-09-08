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
    // Depth of the sliding sum, which is not the ring's depth: the writer runs
    // kReadDelay entries ahead of the aging index, so rollingCadenceTicks_
    // always holds this many deltas. Saffire stores the filter output as
    // `v22 >> 8` (0xd73a), i.e. an average over 256, while its ring is 512 --
    // the second half is replay history, not filter depth.
    static constexpr uint32_t kCadenceAverageEntries = kEntryCount / 2;

    // What one SYT did to the delta chain.
    enum class Update : uint8_t {
        kIgnored,   ///< NO_INFO. Not a cadence observation; the chain is intact.
        kSeeded,    ///< Chain start or restart. The ring absorbed the running
                    ///< average, as Saffire's ReadFirewireBuffers does at 0xd6ac.
        kExtended,  ///< A forward delta was appended.
    };

    struct Snapshot final {
        uint32_t epoch{0};
        uint16_t writeIndex{0};
        uint32_t validUpdates{0};
        uint32_t rollingCadenceTicks{0};
        uint32_t seedCount{0};
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
        seedCount_.store(0, std::memory_order_relaxed);
        previousSyt_.store(kNoInfo, std::memory_order_relaxed);
        recoveredPhaseTicks_.store(0, std::memory_order_relaxed);
        established_.store(false, std::memory_order_relaxed);
        epoch_.fetch_add(1, std::memory_order_relaxed);
        EndWrite();
    }

    // `packetCycleTimer` is the cycle-timer estimate for the packet carrying
    // this SYT, not the later drain/callback instant.
    Update Observe(uint16_t syt, uint32_t packetCycleTimer) noexcept {
        if (syt == kNoInfo) {
            return Update::kIgnored;
        }

        BeginWrite();

        const uint16_t previous = previousSyt_.load(std::memory_order_relaxed);
        // SYTDiffInOffsets reduces into the 16-cycle SYT field domain and
        // returns the shortest signed path, so its range is (-24576, +24576].
        // The nominal step is 4096 ticks at every supported rate.
        const int64_t delta = previous == kNoInfo
            ? 0
            : ASFW::Timing::SYTDiffInOffsets(syt, previous);
        const bool extends = previous != kNoInfo && delta > 0;

        // Seed, restart and a non-advancing SYT all take one path, because the
        // reference takes one action for all three: write the ring's own
        // current running average and count it (Saffire ReadFirewireBuffers
        // 0xd6ac). Being rate-adaptive by construction, it carries none of the
        // bias a nominal constant would at a rate whose real step differs.
        //
        // This replaces two divergences from that reference at once.
        //
        // The seed used to write nothing and count nothing, which desynchronised
        // the writer from any reader trailing by kReadDelay by one entry per
        // seed. That was documented as harmless "only while no such reader
        // exists"; the cursor correction below is such a reader.
        //
        // And the old plausibility band -- `delta <= 0 || delta > UINT16_MAX`,
        // escalated by the caller to a full replay-epoch reset -- was half dead
        // and half destructive. The upper half could never fire: the domain
        // above caps |delta| at 24576, well under UINT16_MAX. The lower half
        // fires on a genuine event, but not the one it names: at 4096 ticks per
        // packet the signed domain wraps after six lost packets, so a burst loss
        // aliases to a negative delta. Tearing down the replay epoch for that is
        // strictly worse than reseeding the chain, and the reference does not do
        // it -- Saffire's only chain break is a DBC mismatch (0xd67c). Loss is
        // now detected where it can be measured rather than guessed, by the
        // phase-versus-cursor comparison; see FramesForPhaseDelta.
        const uint16_t incoming = extends
            ? static_cast<uint16_t>(delta)
            : static_cast<uint16_t>(
                  rollingCadenceTicks_.load(std::memory_order_relaxed) /
                  kCadenceAverageEntries);

        const uint16_t writeIndex = writeIndex_.load(std::memory_order_relaxed);
        const uint16_t agingIndex = agingIndex_.load(std::memory_order_relaxed);
        const uint16_t outgoing = entries_[agingIndex].load(std::memory_order_relaxed);

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

        // An observation, not a link in the delta chain, so it is refreshed on
        // the seed path too. Leaving it stale across a restart would hand the
        // cursor correction a phase from before the break.
        int64_t recovered =
            ASFW::Timing::extendTstampFromCycleTimer(packetCycleTimer, syt);
        recovered = ASFW::Timing::normalizeOffsetDomain(recovered);
        recoveredPhaseTicks_.store(recovered, std::memory_order_relaxed);
        if (!extends) {
            seedCount_.fetch_add(1, std::memory_order_relaxed);
        }

        EndWrite();
        return extends ? Update::kExtended : Update::kSeeded;
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
            snapshot.seedCount = seedCount_.load(std::memory_order_relaxed);
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
    std::atomic<uint32_t> seedCount_{0};
    std::atomic<uint16_t> previousSyt_{kNoInfo};
    std::atomic<int64_t> recoveredPhaseTicks_{0};
    std::atomic<bool> established_{false};
};

// Frames spanned by a signed SYT phase delta, at the recovered rate.
//
// Saffire's FillFirewireBuffers does this to place its transmit content
// (0xec78-0xed72): scale the phase difference by the frames a data packet
// carries, then divide by the ring's running average of ticks per data packet.
// The <<8 in the reference is the fixed-point form of dividing by a sum of 256
// entries rather than by its mean, so no separate scaling step is needed here.
//
// `rollingCadenceTicks` is the sliding sum, not the mean -- it already carries
// the factor of kCadenceAverageEntries. Returns false rather than guessing when
// the ring has no rate yet.
//
// This is the term the anchor never had. The SYT of a blocking stream advances
// by a fixed step per data packet, so phase and frame count are two independent
// measurements of the same quantity: any gap between them is frames that did
// not arrive. Deriving the cursor from the phase is therefore not a rate
// correction -- a device whose SYT is a pure function of its frame counter
// contributes exactly zero rate information, and the Apogee Duet measurably is
// one -- it is the only loss detector in the receive path that costs nothing.
[[nodiscard]] inline bool FramesForPhaseDelta(int64_t phaseDeltaTicks,
                                              uint32_t rollingCadenceTicks,
                                              uint32_t framesPerDataPacket,
                                              int64_t& outFrames) noexcept {
    if (rollingCadenceTicks == 0 || framesPerDataPacket == 0) {
        return false;
    }
    const int64_t scaled =
        phaseDeltaTicks * static_cast<int64_t>(framesPerDataPacket) *
        static_cast<int64_t>(RxSytCadence::kCadenceAverageEntries);
    const int64_t divisor = static_cast<int64_t>(rollingCadenceTicks);
    // Round to nearest, symmetric about zero, so a lagging and a leading cursor
    // are corrected by the same magnitude.
    outFrames = scaled >= 0 ? (scaled + divisor / 2) / divisor
                            : -((-scaled + divisor / 2) / divisor);
    return true;
}

static_assert((RxSytCadence::kEntryCount &
               (RxSytCadence::kEntryCount - 1)) == 0);
static_assert(RxSytCadence::kCadenceAverageEntries == RxSytCadence::kReadDelay,
              "The sliding sum spans exactly the gap the replay reader trails "
              "by; both are the ring's half, and the arithmetic below divides "
              "by the sum's depth, not the ring's");
static_assert(std::atomic<int64_t>::is_always_lock_free);

} // namespace ASFW::Driver
