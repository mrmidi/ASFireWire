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
                            RxSequenceEntry& out) const noexcept {
        const Slot& slot = slots_[cursor % kCapacity];
        const uint64_t expectedSequence = cursor + 1;
        if (slot.sequence.load(std::memory_order_acquire) !=
            expectedSequence) {
            return false;
        }
        if (slot.epoch.load(std::memory_order_relaxed) !=
            expectedEpoch) {
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
        if (slot.sequence.load(std::memory_order_relaxed) !=
                expectedSequence ||
            slot.epoch.load(std::memory_order_relaxed) !=
                expectedEpoch) {
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
        RxSequenceEntry& out) noexcept {
        if (!active_ || replay.Epoch() != epoch_ ||
            !replay.Read(nextCursor_, epoch_, out)) {
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
