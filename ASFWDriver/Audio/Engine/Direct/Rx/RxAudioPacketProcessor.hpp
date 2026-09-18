#pragma once

#include "../DirectInputWriter.hpp"
#include "DirectRxTypes.hpp"
#include "../../../Wire/AMDTP/AmdtpTypes.hpp"
#include "../../../Wire/MOTU/MotuPortLayout.hpp"

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
    /// What the CIP header claimed, always — some devices lie here, which is exactly
    /// what makes it worth reporting separately from the stride we trusted.
    uint8_t dbs{0};
    /// The block stride the decode actually used, in quadlets. Equals `dbs` unless a
    /// wrong-DBS quirk or a non-quadlet block layout overrode it.
    uint32_t strideQuadlets{0};
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
    // `trustConfiguredStride`: Loud OXFW units stamp an unreliable dbs in
    // device->host packets; when set, `am824Slots` is the stride authority and
    // the header dbs is surfaced in the result for telemetry only (Linux
    // snd-oxfw SND_OXFW_QUIRK_WRONG_DBS; amdtp-stream.c:766-769).
    [[nodiscard]] RxAudioPacketProcessorResult ProcessPacket(const uint8_t* payload,
                                                             size_t length,
                                                             uint64_t absoluteFrame,
                                                             uint32_t channels,
                                                             uint32_t am824Slots,
                                                             ASFW::Encoding::AudioWireFormat format,
                                                             uint32_t channelOffset = 0,
                                                             bool publishTimeline = true,
                                                             bool trustConfiguredStride = false,
                                                             // MOTU only: PCM chunks this
                                                             // direction carries per data
                                                             // block. Ignored by the
                                                             // quadlet-slot formats, whose
                                                             // unit count is am824Slots.
                                                             uint32_t motuPcmChunks = 0,
                                                             ::ASFW::Encoding::Motu::MotuPortMap motuPorts = {}) noexcept;

private:
    DirectInputWriter& writer_;
};

} // namespace ASFW::AudioEngine::Direct::Rx
