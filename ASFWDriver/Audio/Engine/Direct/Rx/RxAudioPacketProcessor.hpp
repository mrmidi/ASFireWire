#pragma once

#include "../DirectInputWriter.hpp"
#include "DirectRxTypes.hpp"
#include "RxCaptureChannelMap.hpp"
#include "../../../Wire/AMDTP/AmdtpTypes.hpp"
#include "../../../Wire/AM824/MpxMidiDemux.hpp"
#include "../../../Ports/IMidiByteSink.hpp"

#include <cstdint>
#include <cstddef>

namespace ASFW::Audio {
class IRxPayloadCodec;
}

namespace ASFW::AudioEngine::Direct::Rx {

/// Where, and how, to lift MPX-MIDI bytes out of a packet's data blocks.
///
/// Optional: a default-constructed value disables extraction, which is what
/// every endpoint without a MIDI service gets. The processor reads MIDI from the
/// wire payload itself and never from what the PCM writer managed to bind, so it
/// works while no CoreAudio client holds the input buffer.
struct RxMidiExtraction final {
    ASFW::Audio::Ports::IMidiByteSink* sink{nullptr};
    ASFW::Encoding::MpxMidiGeometry geometry{};
    ASFW::Encoding::MpxMidiDemuxCounters* counters{nullptr};
    /// The drain this packet belongs to. Each packet is dated from its own
    /// descriptor timestamp against this (cycle, host) pair, so MIDI time does
    /// not depend on the audio drain cadence.
    uint32_t drainCycleTimer{0};
    uint64_t drainHostTicks{0};

    [[nodiscard]] bool Enabled() const noexcept {
        return sink != nullptr && geometry.Valid();
    }
};

struct RxAudioPacketProcessorResult final {
    DirectRxWriteStatus status{DirectRxWriteStatus::kUnavailable};
    uint32_t framesDecoded{0};
    bool hasValidCip{false};
    bool hasReceiveCycleTimestamp{false};
    uint16_t receiveCycleTimestamp{0};
    uint16_t syt{0xFFFF};
    uint8_t fdf{0};
    /// What the CIP header claimed, always — some devices lie here, which is exactly
    /// what makes it worth reporting separately from the stride we trusted.
    uint8_t dbs{0};
    /// The block stride the decode actually used, in quadlets. Equals `dbs` unless a
    /// wrong-DBS quirk or a non-quadlet block layout overrode it.
    uint32_t strideQuadlets{0};
    uint8_t dbc{0};
    bool mapRejected{false};
    /// MPX-MIDI bytes handed to the sink out of this packet.
    uint32_t midiBytesDelivered{0};
    /// The packet could not be trusted for MIDI (unusable header, truncated or
    /// ragged payload): any bytes it carried are lost, so the sink was told the
    /// stream is discontinuous.
    bool midiGap{false};
};

class RxAudioPacketProcessor final {
public:
    explicit RxAudioPacketProcessor(DirectInputWriter& writer) noexcept
        : writer_(writer) {}

    [[nodiscard]] RxAudioPacketProcessorResult ProcessPacket(
        const uint8_t* payload,
        size_t length,
        uint64_t absoluteFrame,
        uint32_t channels,
        const ::ASFW::Audio::IRxPayloadCodec& codec,
        uint32_t channelOffset = 0,
        bool publishTimeline = true,
        const RxCaptureChannelMap& captureMap = {},
        bool primeDelayLine = false,
        ::ASFW::Encoding::AudioPacketFraming framing =
            ::ASFW::Encoding::AudioPacketFraming::kCip,
        const RxMidiExtraction& midi = {}) noexcept;

private:
    DirectInputWriter& writer_;
};

} // namespace ASFW::AudioEngine::Direct::Rx
