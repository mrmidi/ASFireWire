#pragma once

#include "AmdtpPacketTimeline.hpp"
#include "AmdtpTypes.hpp"
#include "../../Ports/IWirePayloadCodec.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Protocols::Audio::AMDTP {

struct AmdtpPayloadWriterCounters final {
    std::atomic<uint64_t> framesVisited{0};
    std::atomic<uint64_t> framesWritten{0};
    std::atomic<uint64_t> framesWithoutPacket{0};
    std::atomic<uint64_t> framesOutsidePacket{0};
    // Frames not written because their packet was below the first writable
    // packet (the finality frontier): that packet keeps its armed silence.
    std::atomic<uint64_t> framesMissedFinality{0};
    // S_out headroom: the smallest distance, in packets, between a written
    // frame's packet and the first writable packet, since the last reader
    // took it (exchange with INT64_MAX). INT64_MAX: nothing written.
    std::atomic<int64_t> intervalMinFinalityMarginPackets{INT64_MAX};
};

class AmdtpPayloadWriter final : public ::ASFW::Audio::ITxPayloadWriter {
public:
    AmdtpPayloadWriter() noexcept = default;

    void Configure(const AmdtpStreamConfig& streamConfig,
                   const AmdtpTxPolicy& txPolicy) noexcept;

    void BindTimeline(AmdtpPacketTimeline* timeline) noexcept;

    void WriteFloat32Interleaved(const HostAudioBufferView& hostBuffer,
                                 uint64_t firstWritablePacket) noexcept override;

    [[nodiscard]] const AmdtpPayloadWriterCounters& Counters() const noexcept;
    [[nodiscard]] int64_t TakeMinFinalityMarginPackets() noexcept {
        return counters_.intervalMinFinalityMarginPackets.exchange(INT64_MAX,
                                                                   std::memory_order_relaxed);
    }

private:
    AmdtpStreamConfig streamConfig_{};
    AmdtpTxPolicy txPolicy_{};

    AmdtpPacketTimeline* timeline_{nullptr};
    AmdtpPayloadWriterCounters counters_{};
};

} // namespace ASFW::Protocols::Audio::AMDTP
