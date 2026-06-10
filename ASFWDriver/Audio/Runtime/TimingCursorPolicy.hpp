#pragma once

#include "SaffireIsochLatency.hpp"

#include <cstdint>

namespace ASFW::Audio {

enum class AudioDirection : uint8_t {
    Input = 0,
    Output,
};

enum class AudioTimingMode : uint8_t {
    Dice48kBlocking = 0,
};

struct TimingCursorPolicySnapshot {
    uint32_t sampleRateHz{0};
    uint32_t framesPerPacketMax{0};
    uint32_t outputDelayPackets{0};
    uint32_t inputDelayPackets{0};
    uint32_t outputCursorOffsetFrames{0};
    uint32_t inputCursorOffsetFrames{0};
    uint32_t reportedOutputLatencyFrames{0};
    uint32_t reportedInputLatencyFrames{0};
    uint32_t outputSafetyOffsetFrames{0};
    uint32_t inputSafetyOffsetFrames{0};
    uint32_t outputPacketLeadFrames{0};
    uint32_t inputPacketLeadFrames{0};
    uint32_t ztsPeriodFrames{0};
};

class TimingCursorPolicy {
public:
    static constexpr TimingCursorPolicy MakeDice48kBlocking() noexcept {
        return TimingCursorPolicy(AudioTimingMode::Dice48kBlocking);
    }

    constexpr explicit TimingCursorPolicy(AudioTimingMode mode) noexcept : mode_(mode) {}

    [[nodiscard]] constexpr uint32_t SampleRateHz() const noexcept {
        return 48000;
    }

    [[nodiscard]] constexpr uint32_t FramesPerPacketMax() const noexcept {
        return 8;
    }

    [[nodiscard]] constexpr uint32_t RxAuthorityUpdatePeriodFrames() const noexcept {
        return 8;
    }

    [[nodiscard]] constexpr uint32_t HalIoPeriodFrames() const noexcept {
        return 512;
    }

    [[nodiscard]] constexpr uint32_t HalZeroTimestampPeriodFrames() const noexcept {
        return 512;
    }

    // The Saffire latency mode this policy operates at. Mode kLow = 6/16
    // delay packets, per the UpdateIsochBufferParams trace recommendation
    // (start at mode 0–1 for TX, widen only if frames-without-packet misses
    // climb). See SaffireIsochLatency.hpp for the full lifted model.
    [[nodiscard]] static constexpr SaffireLatencyMode LatencyMode() noexcept {
        return SaffireLatencyMode::kLow;
    }

    [[nodiscard]] static constexpr SaffireIsochBufferParams SaffireParams() noexcept {
        SaffireIsochBufferParams params{};
        (void)SaffireIsochLatency::Lookup(LatencyMode(), 48000, params);
        return params;
    }

    [[nodiscard]] constexpr uint32_t CursorOffsetFrames(AudioDirection direction) const noexcept {
        // Derived from the lifted table: delayPackets × framesPerPacket.
        return (direction == AudioDirection::Output)
                   ? SaffireParams().OutputDelayFrames()
                   : SaffireParams().InputDelayFrames();
    }

    [[nodiscard]] constexpr uint32_t ReportedLatencyFrames(AudioDirection direction) const noexcept {
        // Bench-tuned ADK value, deliberately NOT the table's delay frames:
        // the cursor offset above already absorbs the packet delay; this is
        // what we report on top. Mapping question still open on the bench.
        return (direction == AudioDirection::Output) ? 29 : 0;
    }

    [[nodiscard]] constexpr uint32_t SafetyOffsetFrames(AudioDirection direction) const noexcept {
        return 8; // Saffire-style baseline (bench-tuned ADK value, see above)
    }

    [[nodiscard]] constexpr uint32_t PacketLeadFrames(AudioDirection direction) const noexcept {
        return (direction == AudioDirection::Output) ? 384 : 0; // matching Config::kOutputConsumerLeadFrames
    }

    [[nodiscard]] constexpr uint32_t StartupLeadFrames(AudioDirection direction) const noexcept {
        return PacketLeadFrames(direction);
    }

    [[nodiscard]] constexpr uint32_t PreparationDeadlineFrames(
        AudioDirection direction) const noexcept {
        return (direction == AudioDirection::Output) ? 384 : 0;
    }

    [[nodiscard]] constexpr uint32_t CursorResyncDeadbandFrames() const noexcept {
        return 64; // matching Config::kOutputCursorResyncDeadbandFrames
    }

    [[nodiscard]] constexpr uint32_t ZtsPeriodFrames() const noexcept {
        return 8; // publication period matches frames per packet
    }

    [[nodiscard]] constexpr uint64_t HardwareOutputFrameToReportedFrame(uint64_t hardwareFrame) const noexcept {
        const uint64_t offset = CursorOffsetFrames(AudioDirection::Output);
        return (hardwareFrame >= offset) ? (hardwareFrame - offset) : 0;
    }

    [[nodiscard]] constexpr uint64_t HostOutputFrameToPlaybackFrame(uint64_t hostFrame) const noexcept {
        const uint64_t offset = CursorOffsetFrames(AudioDirection::Output);
        return hostFrame + offset; // map host write cursor to playback timeline frame
    }

    [[nodiscard]] constexpr uint64_t HardwareInputFrameToCaptureFrame(uint64_t hardwareFrame) const noexcept {
        const uint64_t offset = CursorOffsetFrames(AudioDirection::Input);
        return (hardwareFrame >= offset) ? (hardwareFrame - offset) : 0;
    }

    [[nodiscard]] constexpr TimingCursorPolicySnapshot Snapshot() const noexcept {
        return TimingCursorPolicySnapshot{
            .sampleRateHz = SampleRateHz(),
            .framesPerPacketMax = FramesPerPacketMax(),
            .outputDelayPackets = SaffireParams().outputDelayPackets,
            .inputDelayPackets = SaffireParams().inputDelayPackets,
            .outputCursorOffsetFrames = CursorOffsetFrames(AudioDirection::Output),
            .inputCursorOffsetFrames = CursorOffsetFrames(AudioDirection::Input),
            .reportedOutputLatencyFrames = ReportedLatencyFrames(AudioDirection::Output),
            .reportedInputLatencyFrames = ReportedLatencyFrames(AudioDirection::Input),
            .outputSafetyOffsetFrames = SafetyOffsetFrames(AudioDirection::Output),
            .inputSafetyOffsetFrames = SafetyOffsetFrames(AudioDirection::Input),
            .outputPacketLeadFrames = PacketLeadFrames(AudioDirection::Output),
            .inputPacketLeadFrames = PacketLeadFrames(AudioDirection::Input),
            .ztsPeriodFrames = ZtsPeriodFrames()
        };
    }

private:
    AudioTimingMode mode_;
};

// The table derivation must reproduce the previously hand-planted values
// exactly — this change is single-sourcing, not retuning.
static_assert(TimingCursorPolicy::SaffireParams().OutputDelayFrames() == 48,
              "mode kLow output delay at 48 kHz must stay 6 packets * 8 frames");
static_assert(TimingCursorPolicy::SaffireParams().InputDelayFrames() == 128,
              "mode kLow input delay at 48 kHz must stay 16 packets * 8 frames");
static_assert(TimingCursorPolicy::SaffireParams().outputDelayPackets == 6 &&
                  TimingCursorPolicy::SaffireParams().inputDelayPackets == 16,
              "snapshot delay packets must match the lifted table at mode kLow");

} // namespace ASFW::Audio
