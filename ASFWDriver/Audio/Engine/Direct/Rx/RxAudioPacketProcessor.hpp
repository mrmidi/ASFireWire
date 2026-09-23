#pragma once

#include "../DirectInputWriter.hpp"
#include "DirectRxTypes.hpp"
#include "RxCaptureChannelMap.hpp"
#include "../../../Wire/AMDTP/AmdtpTypes.hpp"

#include <cstdint>
#include <cstddef>

namespace ASFW::Audio {
class IRxPayloadCodec;
}

namespace ASFW::AudioEngine::Direct::Rx {

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
        bool emptyPacketHasWrongDbc = false,
        bool previousDbcValid = false,
        uint8_t previousDbc = 0) noexcept;

private:
    DirectInputWriter& writer_;
};

} // namespace ASFW::AudioEngine::Direct::Rx
