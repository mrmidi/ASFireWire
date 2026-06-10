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
};

class AmdtpPayloadWriter final {
public:
    AmdtpPayloadWriter() noexcept = default;

    void Configure(const AmdtpStreamConfig& streamConfig,
                   const AmdtpTxPolicy& txPolicy) noexcept;

    void BindTimeline(AmdtpPacketTimeline* timeline) noexcept;

    void WriteFloat32Interleaved(const HostAudioBufferView& hostBuffer) noexcept;

    [[nodiscard]] const AmdtpPayloadWriterCounters& Counters() const noexcept;

private:
    AmdtpStreamConfig streamConfig_{};
    AmdtpTxPolicy txPolicy_{};

    AmdtpPacketTimeline* timeline_{nullptr};
    AmdtpPayloadWriterCounters counters_{};
};

} // namespace ASFW::Protocols::Audio::AMDTP