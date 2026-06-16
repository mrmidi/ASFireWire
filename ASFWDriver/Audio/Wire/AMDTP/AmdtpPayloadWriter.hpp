#pragma once

#include "AmdtpPacketTimeline.hpp"
#include "AmdtpTypes.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

struct AmdtpPayloadWriterCounters final {
    std::atomic<uint64_t> framesVisited{0};
    std::atomic<uint64_t> framesWritten{0};
    std::atomic<uint64_t> framesWithoutPacket{0};
    std::atomic<uint64_t> framesOutsidePacket{0};
    // Overlap diagnostic, not a partition bucket: frames already counted in
    // framesWritten whose slot was reused while the payload was being
    // written (detected by the post-write generation recheck).
    std::atomic<uint64_t> framesRacedReuse{0};
    // Frames whose packetIndex was already retired by hardware (≤
    // completionCursor) at write time — these writes land in the past and
    // never reach the wire.
    std::atomic<uint64_t> framesWroteIntoTransmitted{0};
    std::atomic<uint64_t> framesNonZero{0};
    std::atomic<uint64_t> slotsNonZero{0};
    std::atomic<uint64_t> underExposureCalls{0};
    std::atomic<uint64_t> underExposureFrames{0};
    std::atomic<uint32_t> maxAbsSampleBits{0}; // Absolute int32 sample magnitude
};

class AmdtpPayloadWriter final {
public:
    AmdtpPayloadWriter() noexcept = default;

    void Configure(const AmdtpStreamConfig& streamConfig,
                   const AmdtpTxPolicy& txPolicy) noexcept;

    void BindTimeline(AmdtpPacketTimeline* timeline) noexcept;

    void WriteFloat32Interleaved(const HostAudioBufferView& hostBuffer,
                                 uint64_t completionCursor) noexcept;

    [[nodiscard]] const AmdtpPayloadWriterCounters& Counters() const noexcept;

private:
    AmdtpStreamConfig streamConfig_{};
    AmdtpTxPolicy txPolicy_{};

    AmdtpPacketTimeline* timeline_{nullptr};
    AmdtpPayloadWriterCounters counters_{};
};

} // namespace ASFW::Protocols::Audio::AMDTP
