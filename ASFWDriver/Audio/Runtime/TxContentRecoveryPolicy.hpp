#pragma once

#include <cstdint>
#include <limits>

namespace ASFW::Audio::Runtime {

// Classification is deliberately independent of DriverKit and the DICE
// backend.  It is the audio coordinator's policy for a value-owned PCM
// snapshot that could not be obtained before packet publication.
enum class TxContentShortageKind : uint8_t {
    kNotYetWritten = 0,
    kSnapshotBusy,
    kStaleOverwritten,
};

enum class TxContentShortageAction : uint8_t {
    kDefer = 0,
    kEmitNoDataHoldCursor,
    kEmitNoDataRebaseCursor,
};

[[nodiscard]] constexpr TxContentShortageAction
DecideTxContentShortageAction(TxContentShortageKind shortage,
                              bool mustCoverTransport) noexcept {
    switch (shortage) {
        case TxContentShortageKind::kNotYetWritten:
        case TxContentShortageKind::kSnapshotBusy:
            // Optional work can wait.  At the hard transport boundary we must
            // publish a packet, but the missing PCM is still recoverable: keep
            // its cursor so the next DATA opportunity retries the same range.
            return mustCoverTransport
                       ? TxContentShortageAction::kEmitNoDataHoldCursor
                       : TxContentShortageAction::kDefer;
        case TxContentShortageKind::kStaleOverwritten:
            // Waiting cannot recover a range that has left retention.
            return TxContentShortageAction::kEmitNoDataRebaseCursor;
    }
    return TxContentShortageAction::kEmitNoDataHoldCursor;
}

struct TxCompletePcmPacketSelection final {
    bool available{false};
    uint64_t firstFrame{0};
};

// Clamp a presentation-derived frame onto a packet-aligned range that is
// wholly contained in the staging ring's retained interval.  Selecting the
// producer frontier itself is unsafe when only a prefix of the next DATA
// packet has been published.
[[nodiscard]] constexpr TxCompletePcmPacketSelection
SelectCompletePcmPacket(uint64_t projectedFrame,
                        uint64_t oldestRetainedFrame,
                        uint64_t writtenEndFrame,
                        uint32_t framesPerPacket) noexcept {
    if (framesPerPacket == 0 ||
        oldestRetainedFrame > writtenEndFrame ||
        writtenEndFrame < framesPerPacket) {
        return {};
    }

    const uint64_t packetFrames = framesPerPacket;
    const uint64_t latestAligned =
        ((writtenEndFrame - packetFrames) / packetFrames) * packetFrames;

    uint64_t earliestAligned = oldestRetainedFrame;
    const uint64_t oldestRemainder = earliestAligned % packetFrames;
    if (oldestRemainder != 0) {
        const uint64_t advance = packetFrames - oldestRemainder;
        if (earliestAligned >
            std::numeric_limits<uint64_t>::max() - advance) {
            return {};
        }
        earliestAligned += advance;
    }
    if (earliestAligned > latestAligned) {
        return {};
    }

    const uint64_t projectedAligned =
        (projectedFrame / packetFrames) * packetFrames;
    const uint64_t selected =
        projectedAligned < earliestAligned
            ? earliestAligned
            : (projectedAligned > latestAligned
                   ? latestAligned
                   : projectedAligned);
    return {.available = true, .firstFrame = selected};
}

} // namespace ASFW::Audio::Runtime
