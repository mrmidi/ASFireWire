#pragma once

#include <cstdint>

namespace ASFW::Driver {

// Saffire-style per-callback FireWire phase continuity tracker.
//
// Port of FillFirewireBuffers 0xe9bf-0xed72: every callback, read the actual
// DCL/OHCI timestamp, convert to offset ticks, compare against the previously
// predicted callback phase, and reset the output phase if continuity breaks.
// One-cycle glitch tolerance (diff == 3072, count <= 2) matches the decompiled
// Saffire behavior at 0x194103 / 0x19410B.
//
// This is NOT ppm compensation. The FireWire cycle-timer domain is the
// coordinate system; 3072 ticks per cycle is exact by definition. The tracker
// detects when the actual callback phase diverges from the prediction, which
// indicates a real discontinuity (dropout, reconnection, scheduling glitch)
// rather than clock drift.
class TxAnchorTracker final {
public:
    static constexpr int64_t kPhaseModTicks = 196'608'000;  // 8-second domain
    static constexpr int64_t kTicksPerCycle = 3072;

    enum class Status : uint8_t {
        kAccepted = 0,
        kOneCycleGlitchAccepted,
        kDiscontinuityReset,
    };

    struct Result final {
        Status status{Status::kAccepted};
        int64_t callbackPhaseTicks{0};
        int64_t expectedPhaseTicks{0};
        int64_t continuityDiffTicks{0};
        bool resetRequired{false};
    };

    [[nodiscard]] static int64_t Normalize(int64_t ticks) noexcept {
        ticks %= kPhaseModTicks;
        if (ticks < 0) {
            ticks += kPhaseModTicks;
        }
        return ticks;
    }

    [[nodiscard]] static int64_t Diff(int64_t a, int64_t b) noexcept {
        int64_t d = (a - b) % kPhaseModTicks;
        if (d < 0) {
            d += kPhaseModTicks;
        }
        if (d > kPhaseModTicks / 2) {
            d -= kPhaseModTicks;
        }
        return d;
    }

    [[nodiscard]] Result ObserveCallbackPhase(uint64_t completedPacketIndex, int64_t observedPhaseTicks) noexcept {
        Result result{};
        result.callbackPhaseTicks = Normalize(observedPhaseTicks);

        if (!valid_) {
            valid_ = true;
            oneCycleGlitchCount_ = 0;
            lastCompletedPacketIndex_ = completedPacketIndex;
            lastCompletedPhaseTicks_ = result.callbackPhaseTicks;
            result.expectedPhaseTicks = result.callbackPhaseTicks;
            result.continuityDiffTicks = 0;
            result.status = Status::kAccepted;
            return result;
        }

        const int64_t packetAdvance =
            static_cast<int64_t>(completedPacketIndex - lastCompletedPacketIndex_);
        const int64_t expectedPhaseTicks =
            Normalize(lastCompletedPhaseTicks_ + packetAdvance * kTicksPerCycle);

        result.expectedPhaseTicks = expectedPhaseTicks;
        result.continuityDiffTicks =
            Diff(result.callbackPhaseTicks, expectedPhaseTicks);

        if (result.continuityDiffTicks == 0) {
            oneCycleGlitchCount_ = 0;
            lastCompletedPacketIndex_ = completedPacketIndex;
            lastCompletedPhaseTicks_ = result.callbackPhaseTicks;
            result.status = Status::kAccepted;
            return result;
        }

        ++oneCycleGlitchCount_;

        if (result.continuityDiffTicks == kTicksPerCycle &&
            oneCycleGlitchCount_ <= 2) {
            // One-cycle glitch: rebase actual to predicted, update prediction
            // from rebased point. Matches Saffire 0x194103.
            result.callbackPhaseTicks = expectedPhaseTicks;
            lastCompletedPacketIndex_ = completedPacketIndex;
            lastCompletedPhaseTicks_ = expectedPhaseTicks;
            result.status = Status::kOneCycleGlitchAccepted;
            return result;
        }

        // Real discontinuity: reset. Matches Saffire 0x19410B.
        valid_ = false;
        oneCycleGlitchCount_ = 0;
        lastCompletedPacketIndex_ = 0;
        lastCompletedPhaseTicks_ = 0;
        result.resetRequired = true;
        result.status = Status::kDiscontinuityReset;
        return result;
    }

    void Reset() noexcept {
        valid_ = false;
        oneCycleGlitchCount_ = 0;
        lastCompletedPacketIndex_ = 0;
        lastCompletedPhaseTicks_ = 0;
    }

    [[nodiscard]] bool IsValid() const noexcept { return valid_; }

private:
    bool valid_{false};
    uint32_t oneCycleGlitchCount_{0};
    uint64_t lastCompletedPacketIndex_{0};
    int64_t lastCompletedPhaseTicks_{0};
};

} // namespace ASFW::Driver
