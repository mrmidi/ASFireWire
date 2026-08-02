#pragma once

#include "AmdtpTiming.hpp"

#include <array>
#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

struct RxSequenceEntry final {
    uint64_t firstAudioFrame{0};
    uint32_t sourceCycleTimer{0};
    uint32_t sytOffset{UINT32_MAX};
    uint16_t dataBlocks{0};
    uint8_t dbc{0};
    uint8_t flags{0};
};

namespace RxSequenceFlags {
inline constexpr uint8_t kValidCip = 1u << 0;
inline constexpr uint8_t kValidSyt = 1u << 1;
inline constexpr uint8_t kDiscontinuity = 1u << 2;
} // namespace RxSequenceFlags

// A false replay read is recoverable, but its cause is not interchangeable:
// future consumption means TX is ahead of RX; a stale cursor means history was
// overwritten; an epoch change is an RX discontinuity.  Preserve that boundary
// at the reader so the TX recovery path can report what it actually saw.
enum class RxSequenceReplayReadFailure : uint8_t {
    kNone,
    kReaderInactive,
    kEpochChanged,
    kAheadOfProducer,
    kHistoryOverwritten,
    kSlotSequenceMismatch,
    kSlotEpochMismatch,
    kSlotChanged,
};

[[nodiscard]] constexpr const char* RxSequenceReplayReadFailureName(
    RxSequenceReplayReadFailure failure) noexcept {
    switch (failure) {
    case RxSequenceReplayReadFailure::kNone: return "none";
    case RxSequenceReplayReadFailure::kReaderInactive: return "inactive";
    case RxSequenceReplayReadFailure::kEpochChanged: return "epoch";
    case RxSequenceReplayReadFailure::kAheadOfProducer: return "ahead";
    case RxSequenceReplayReadFailure::kHistoryOverwritten: return "overwritten";
    case RxSequenceReplayReadFailure::kSlotSequenceMismatch: return "slot-seq";
    case RxSequenceReplayReadFailure::kSlotEpochMismatch: return "slot-epoch";
    case RxSequenceReplayReadFailure::kSlotChanged: return "slot-changed";
    }
    return "unknown";
}

struct RxSequenceReplayReadDiagnostic final {
    RxSequenceReplayReadFailure failure{RxSequenceReplayReadFailure::kNone};
    uint64_t readerCursor{0};
    uint64_t producerCursor{0};
    uint64_t slotSequence{0};
    uint32_t readerEpoch{0};
    uint32_t replayEpoch{0};
    uint32_t slotEpoch{0};
    bool replayEstablished{false};
};

[[nodiscard]] inline uint32_t ComputeReplaySytOffset(
    uint16_t syt,
    uint32_t sourceCycleTimer,
    uint32_t transferDelayTicks) noexcept {
    constexpr uint32_t kSytCycleModulus = 16;
    constexpr uint32_t kSytNoInfo = UINT16_MAX;
    if (syt == kSytNoInfo) {
        return UINT32_MAX;
    }

    const uint32_t sourceCycle =
        ASFW::Timing::decodeCycleTimer(sourceCycleTimer).cycle;
    const uint32_t sourceCycleLow = sourceCycle & 0x0Fu;
    uint32_t sytCycleLow = (static_cast<uint32_t>(syt) >> 12) & 0x0Fu;
    if (sytCycleLow < sourceCycleLow) {
        sytCycleLow += kSytCycleModulus;
    }
    sytCycleLow -= sourceCycleLow;

    uint32_t offset =
        sytCycleLow * ASFW::Timing::kTicksPerCycle +
        (static_cast<uint32_t>(syt) & 0x0FFFu);
    if (offset < transferDelayTicks) {
        offset +=
            kSytCycleModulus * ASFW::Timing::kTicksPerCycle;
    }
    return offset - transferDelayTicks;
}

[[nodiscard]] inline uint16_t ComputeReplaySyt(
    uint32_t sytOffset,
    uint32_t outputCycleTimer,
    uint32_t transferDelayTicks) noexcept {
    if (sytOffset == UINT32_MAX) {
        return UINT16_MAX;
    }

    const uint32_t outputCycle =
        ASFW::Timing::decodeCycleTimer(outputCycleTimer).cycle;
    const uint32_t presentationOffset = sytOffset + transferDelayTicks;
    const uint32_t cycle =
        outputCycle +
        presentationOffset / ASFW::Timing::kTicksPerCycle;
    const uint32_t offset =
        presentationOffset % ASFW::Timing::kTicksPerCycle;
    return static_cast<uint16_t>(
        ((cycle & 0x0Fu) << 12) | offset);
}

[[nodiscard]] inline uint16_t ComputeReplaySytFromTicks(
    uint32_t sytOffset,
    int64_t outputCycleTicks,
    uint32_t transferDelayTicks) noexcept {
    const int64_t normalized =
        ASFW::Timing::normalizeOffsetDomain(outputCycleTicks);
    const uint32_t seconds = static_cast<uint32_t>(
        normalized / static_cast<int64_t>(ASFW::Timing::kTicksPerSecond));
    const uint32_t ticksWithinSecond = static_cast<uint32_t>(
        normalized % static_cast<int64_t>(ASFW::Timing::kTicksPerSecond));
    const uint32_t cycle =
        ticksWithinSecond / ASFW::Timing::kTicksPerCycle;
    const uint32_t offset =
        ticksWithinSecond % ASFW::Timing::kTicksPerCycle;
    return ComputeReplaySyt(
        sytOffset,
        ASFW::Timing::encodeCycleTimer(seconds, cycle, offset),
        transferDelayTicks);
}

class RxSequenceReplayState final {
public:
    static constexpr uint32_t kCapacity = 512;
    static constexpr uint32_t kReadDelay = kCapacity / 2;
    static constexpr uint32_t kNoInfo = UINT32_MAX;

    struct Slot final {
        std::atomic<uint64_t> sequence{0};
        std::atomic<uint64_t> firstAudioFrame{0};
        std::atomic<uint32_t> epoch{0};
        std::atomic<uint32_t> sourceCycleTimer{0};
        std::atomic<uint32_t> sytOffset{kNoInfo};
        std::atomic<uint16_t> dataBlocks{0};
        std::atomic<uint8_t> dbc{0};
        std::atomic<uint8_t> flags{0};
    };

    void Reset() noexcept {
        established_.store(false, std::memory_order_release);
        producerCursor_.store(0, std::memory_order_relaxed);
        epoch_.fetch_add(1, std::memory_order_acq_rel);
        for (auto& slot : slots_) {
            slot.sequence.store(0, std::memory_order_relaxed);
        }
    }

    void Publish(const RxSequenceEntry& entry) noexcept {
        const uint64_t cursor =
            producerCursor_.load(std::memory_order_relaxed);
        Slot& slot = slots_[cursor % kCapacity];
        slot.epoch.store(Epoch(), std::memory_order_relaxed);
        slot.firstAudioFrame.store(
            entry.firstAudioFrame, std::memory_order_relaxed);
        slot.sourceCycleTimer.store(
            entry.sourceCycleTimer, std::memory_order_relaxed);
        slot.sytOffset.store(entry.sytOffset, std::memory_order_relaxed);
        slot.dataBlocks.store(entry.dataBlocks, std::memory_order_relaxed);
        slot.dbc.store(entry.dbc, std::memory_order_relaxed);
        slot.flags.store(entry.flags, std::memory_order_relaxed);
        slot.sequence.store(cursor + 1, std::memory_order_release);
        producerCursor_.store(cursor + 1, std::memory_order_release);
    }

    [[nodiscard]] bool MarkEstablished() noexcept {
        if (ProducerCursor() < kReadDelay) {
            return false;
        }
        established_.store(true, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool IsEstablished() const noexcept {
        return established_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint64_t ProducerCursor() const noexcept {
        return producerCursor_.load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t Epoch() const noexcept {
        return epoch_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Read(uint64_t cursor,
                            uint32_t expectedEpoch,
                            RxSequenceEntry& out,
                            RxSequenceReplayReadDiagnostic* diagnostic = nullptr) const noexcept {
        const Slot& slot = slots_[cursor % kCapacity];
        const uint64_t expectedSequence = cursor + 1;
        const uint64_t firstSequence =
            slot.sequence.load(std::memory_order_acquire);
        if (diagnostic) {
            diagnostic->slotSequence = firstSequence;
        }
        if (firstSequence != expectedSequence) {
            if (diagnostic) {
                diagnostic->failure =
                    RxSequenceReplayReadFailure::kSlotSequenceMismatch;
            }
            return false;
        }
        const uint32_t firstEpoch =
            slot.epoch.load(std::memory_order_relaxed);
        if (diagnostic) {
            diagnostic->slotEpoch = firstEpoch;
        }
        if (firstEpoch != expectedEpoch) {
            if (diagnostic) {
                diagnostic->failure =
                    RxSequenceReplayReadFailure::kSlotEpochMismatch;
            }
            return false;
        }

        RxSequenceEntry entry{};
        entry.firstAudioFrame =
            slot.firstAudioFrame.load(std::memory_order_relaxed);
        entry.sourceCycleTimer =
            slot.sourceCycleTimer.load(std::memory_order_relaxed);
        entry.sytOffset =
            slot.sytOffset.load(std::memory_order_relaxed);
        entry.dataBlocks =
            slot.dataBlocks.load(std::memory_order_relaxed);
        entry.dbc = slot.dbc.load(std::memory_order_relaxed);
        entry.flags = slot.flags.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t finalSequence =
            slot.sequence.load(std::memory_order_relaxed);
        const uint32_t finalEpoch =
            slot.epoch.load(std::memory_order_relaxed);
        if (diagnostic) {
            diagnostic->slotSequence = finalSequence;
            diagnostic->slotEpoch = finalEpoch;
        }
        if (finalSequence != expectedSequence ||
            finalEpoch != expectedEpoch) {
            if (diagnostic) {
                diagnostic->failure =
                    finalEpoch != expectedEpoch
                        ? RxSequenceReplayReadFailure::kSlotEpochMismatch
                        : RxSequenceReplayReadFailure::kSlotChanged;
            }
            return false;
        }

        out = entry;
        return true;
    }

private:
    std::array<Slot, kCapacity> slots_{};
    std::atomic<uint64_t> producerCursor_{0};
    std::atomic<uint32_t> epoch_{0};
    std::atomic<bool> established_{false};
};

class RxSequenceReplayReader final {
public:
    void Reset() noexcept {
        nextCursor_ = 0;
        epoch_ = 0;
        active_ = false;
    }

    [[nodiscard]] bool Begin(
        const RxSequenceReplayState& replay) noexcept {
        const uint64_t producer = replay.ProducerCursor();
        if (!replay.IsEstablished() ||
            producer < RxSequenceReplayState::kReadDelay) {
            return false;
        }

        epoch_ = replay.Epoch();
        nextCursor_ =
            producer - RxSequenceReplayState::kReadDelay;
        active_ = true;
        return true;
    }

    [[nodiscard]] bool TryRead(
        const RxSequenceReplayState& replay,
        RxSequenceEntry& out,
        RxSequenceReplayReadDiagnostic* diagnostic = nullptr) noexcept {
        RxSequenceReplayReadDiagnostic ignoredDiagnostic{};
        RxSequenceReplayReadDiagnostic* const observed =
            diagnostic ? diagnostic : &ignoredDiagnostic;
        *observed = {
            .readerCursor = nextCursor_,
            .producerCursor = replay.ProducerCursor(),
            .readerEpoch = epoch_,
            .replayEpoch = replay.Epoch(),
            .replayEstablished = replay.IsEstablished(),
        };
        if (!active_) {
            observed->failure = RxSequenceReplayReadFailure::kReaderInactive;
            return false;
        }
        if (observed->replayEpoch != epoch_) {
            observed->failure = RxSequenceReplayReadFailure::kEpochChanged;
            return false;
        }
        if (nextCursor_ >= observed->producerCursor) {
            observed->failure = RxSequenceReplayReadFailure::kAheadOfProducer;
            return false;
        }
        if (observed->producerCursor - nextCursor_ >
            RxSequenceReplayState::kCapacity) {
            observed->failure = RxSequenceReplayReadFailure::kHistoryOverwritten;
            return false;
        }
        if (!replay.Read(nextCursor_, epoch_, out, observed)) {
            return false;
        }
        ++nextCursor_;
        return true;
    }

    [[nodiscard]] bool IsActive() const noexcept {
        return active_;
    }

    [[nodiscard]] uint64_t NextCursor() const noexcept {
        return nextCursor_;
    }

private:
    uint64_t nextCursor_{0};
    uint32_t epoch_{0};
    bool active_{false};
};

static_assert(
    (RxSequenceReplayState::kCapacity &
     (RxSequenceReplayState::kCapacity - 1)) == 0);

} // namespace ASFW::Audio::Runtime
