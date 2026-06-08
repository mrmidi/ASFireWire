// OutputPhaseToAudioMap.hpp
// ASFW - Maps outputPhaseTicks -> absolute audio sample frame for packet ring lookup.

#pragma once

#include <cstdint>

namespace ASFW::AudioEngine::DirectIsoch {

/// Maps the loop's unwrapped output phase to an absolute audio sample frame.
///
/// The output phase (TxOutputPhaseLoop) is an UNWRAPPED, monotonically increasing tick
/// value, so the mapping is a plain affine transform anchored once at stream start:
///   absoluteFrame = epochAudioFrame + (outputPhaseTicks - epochPhaseTicks) / kTicksPerFrame
///
/// `epochAudioFrame` is seeded at `reportedPlayhead - safety` (NOT at writtenEnd), so the
/// selected packet ranges land inside the written audio window [oldestValid, writtenEnd)
/// with margin. On a device discontinuity the caller Reset()s and re-Anchor()s.
class OutputPhaseToAudioMap {
public:
    static constexpr int64_t kTicksPerFrame = 512;

    void Reset() noexcept {
        anchored_ = false;
        epochPhaseTicks_ = 0;
        epochAudioFrame_ = 0;
    }

    /// Anchor the map: call once when the first stable output phase and the target
    /// audio frame (reportedPlayhead - safety) are known.
    void Anchor(int64_t outputPhaseTicks, uint64_t audioFrame) noexcept {
        epochPhaseTicks_ = outputPhaseTicks;
        epochAudioFrame_ = audioFrame;
        anchored_ = true;
    }

    [[nodiscard]] bool IsAnchored() const noexcept { return anchored_; }

    /// Convert unwrapped output phase ticks to absolute audio frame.
    /// Returns the first frame of the packet that should be read from the ring.
    [[nodiscard]] uint64_t PhaseToFrame(int64_t outputPhaseTicks) const noexcept {
        if (!anchored_) return 0;
        // Plain subtraction on the unwrapped phase: no modular wrap, so the mapping is
        // exact for the full lifetime of the stream (not just a 4-second window).
        const int64_t dtFrames = (outputPhaseTicks - epochPhaseTicks_) / kTicksPerFrame;
        const int64_t frame = static_cast<int64_t>(epochAudioFrame_) + dtFrames;
        return frame < 0 ? 0u : static_cast<uint64_t>(frame);
    }

private:
    bool anchored_{false};
    int64_t epochPhaseTicks_{0};
    uint64_t epochAudioFrame_{0};
};

} // namespace ASFW::AudioEngine::DirectIsoch
