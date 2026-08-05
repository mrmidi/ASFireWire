#pragma once

#include <cstdint>

namespace ASFW::AudioEngine::Direct::Rx {

/// Outcome of decoding one received isochronous packet.
///
/// Values 5..8 split what used to be a single `kInvalidRange`. That collapse
/// made four unrelated faults indistinguishable from outside — a runt packet, a
/// CIP header the decoder rejected, a zero data-block size, and a stream/device
/// geometry disagreement all reported the same number, and all of them end the
/// same way: the replay epoch resets, `allowRecoveredClock` never opens, and the
/// stream stays silent. During bring-up that is also indistinguishable from a
/// device which is genuinely sending only CIP NO-DATA. Keep these separate.
///
/// `kInvalidRange` is retained for its literal meaning: the input writer had no
/// frame for the requested position.
enum class DirectRxWriteStatus : uint32_t {
    kUnavailable = 0,
    kAvailable = 1,
    kInvalidBinding = 2,
    kInvalidRange = 3,
    kOverflow = 4,
    kShortPacket = 5,        ///< Payload too small for the isoch + CIP header.
    kInvalidCipHeader = 6,   ///< CIPHeader::Decode rejected the leading quadlets.
    kZeroDataBlockSize = 7,  ///< CIP DBS == 0, so no event size can be derived.
    kGeometryMismatch = 8,   ///< Stream channels / DBS / AM824 slots disagree.
};

struct DirectRxWriteRequest final {
    uint64_t absoluteFrame{0};
    uint32_t frameCount{0};
    uint32_t channels{0};
};

struct DirectRxWriteResult final {
    DirectRxWriteStatus status{DirectRxWriteStatus::kUnavailable};
    int32_t* firstFramePtr{nullptr};
    uint64_t producedEndFrame{0};
};

} // namespace ASFW::AudioEngine::Direct::Rx
