#pragma once

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

    [[nodiscard]] constexpr uint32_t CursorOffsetFrames(AudioDirection direction) const noexcept {
        return (direction == AudioDirection::Output) ? 48 : 128; // 6 packets * 8 vs 16 packets * 8
    }

    [[nodiscard]] constexpr uint32_t ReportedLatencyFrames(AudioDirection direction) const noexcept {
        return (direction == AudioDirection::Output) ? 29 : 0;
    }

    [[nodiscard]] constexpr uint32_t SafetyOffsetFrames(AudioDirection direction) const noexcept {
        return 8; // Saffire-style baseline
    }

    [[nodiscard]] constexpr uint32_t PacketLeadFrames(AudioDirection direction) const noexcept {
        return (direction == AudioDirection::Output) ? 768 : 0; // matching Config::kOutputConsumerLeadFrames
    }

    [[nodiscard]] constexpr uint32_t CursorResyncDeadbandFrames() const noexcept {
        return 256; // matching Config::kOutputCursorResyncDeadbandFrames
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
            .outputDelayPackets = 6,
            .inputDelayPackets = 16,
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

} // namespace ASFW::Audio
