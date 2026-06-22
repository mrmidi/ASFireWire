#pragma once

#include "../DirectInputWriter.hpp"
#include "DirectRxTypes.hpp"
#include "../../../Wire/AMDTP/AmdtpTypes.hpp"

#include <cstdint>
#include <cstddef>

namespace ASFW::AudioEngine::Direct::Rx {

struct RxAudioPacketProcessorResult final {
    DirectRxWriteStatus status{DirectRxWriteStatus::kUnavailable};
    uint32_t framesDecoded{0};
    bool hasValidCip{false};
    bool hasReceiveCycleTimestamp{false};
    uint16_t receiveCycleTimestamp{0};
    uint16_t syt{0xFFFF};
    uint8_t fdf{0};
    uint8_t dbs{0};
    uint8_t dbc{0};
};

class RxAudioPacketProcessor final {
public:
    explicit RxAudioPacketProcessor(DirectInputWriter& writer) noexcept
        : writer_(writer) {}

    // `channels` is the number of PCM channels THIS stream decodes (its slice),
    // written into the shared interleaved input buffer starting at `channelOffset`
    // (e.g. 0 for the master/first 16-ch slice, 16 for the second). The buffer's
    // full interleave width (stride) is owned by the writer's binding.
    // `publishTimeline` advances the producer cursor/frame counters — only the
    // master stream does this; secondary slices write PCM only.
    [[nodiscard]] RxAudioPacketProcessorResult ProcessPacket(const uint8_t* payload,
                                                             size_t length,
                                                             uint64_t absoluteFrame,
                                                             uint32_t channels,
                                                             uint32_t am824Slots,
                                                             ASFW::Encoding::AudioWireFormat format,
                                                             uint32_t channelOffset = 0,
                                                             bool publishTimeline = true) noexcept;

private:
    DirectInputWriter& writer_;
};

} // namespace ASFW::AudioEngine::Direct::Rx
