#pragma once

#include "../DirectInputWriter.hpp"
#include "DirectRxTypes.hpp"
#include "RxCaptureChannelMap.hpp"
#include "../../../Wire/AMDTP/AmdtpTypes.hpp"
#include "../../../Wire/AM824/MpxMidiDemux.hpp"

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
    /// The capture map did not fit the packet's data block size, so the wire
    /// order was used instead. A silent permutation failure would look exactly
    /// like a correct decode, so it is reported rather than inferred.
    bool mapRejected{false};
    /// MIDI bytes lifted from this packet. Zero is the normal case.
    uint32_t midiBytesDelivered{0};
    /// The packet's payload was not a whole number of data blocks, so the
    /// geometry we believe is not the geometry on the wire. PCM decode has
    /// always truncated silently here; MIDI refuses instead, because a MIDI
    /// slot index derived from the wrong DBS reads a different slot entirely.
    bool ragged{false};
};

/// Optional MIDI extraction for one received packet.
///
/// Passing a null sink disables it entirely, which is what every audio-only
/// caller does.
struct RxMidiExtraction final {
    ASFW::Audio::Ports::IMidiByteSink* sink{nullptr};
    ASFW::Encoding::MpxMidiGeometry geometry{};
    ASFW::Encoding::MpxMidiDemuxCounters* counters{nullptr};

    [[nodiscard]] bool Enabled() const noexcept {
        return sink != nullptr && geometry.Valid();
    }
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
    // `captureMap` reorders wire slots onto channels and may delay a subset of
    // them; the identity map costs nothing and is the default. `primeDelayLine`
    // silences the delayed channels of the frames ahead of `absoluteFrame`, and
    // must be set on the first packet of an epoch so the head of the delay line
    // cannot expose stale buffer content.
    [[nodiscard]] RxAudioPacketProcessorResult ProcessPacket(const uint8_t* payload,
                                                             size_t length,
                                                             uint64_t absoluteFrame,
                                                             uint32_t channels,
                                                             uint32_t am824Slots,
                                                             ASFW::Encoding::AudioWireFormat format,
                                                             uint32_t channelOffset = 0,
                                                             bool publishTimeline = true,
                                                             const RxCaptureChannelMap& captureMap = {},
                                                             bool primeDelayLine = false,
                                                             const RxMidiExtraction& midi = {}) noexcept;

private:
    DirectInputWriter& writer_;
};

} // namespace ASFW::AudioEngine::Direct::Rx
