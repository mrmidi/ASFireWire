#pragma once

#include "Audio/Runtime/HardwareSampleTimeline.hpp"

#include <cstdint>
#include <expected>
#include <limits>
#include <optional>

// Host-side specification, NOT a new driver clock or a runtime recovery path.
// Evidence must come from the physical trace/oracle, never from the candidate
// being checked. See documentation/AUDIO_TIMING_CONTRACT.md for provenance.
namespace ASFW::Testing::AudioTiming {

enum class Rejection {
    NoEpoch,
    StaleEpoch,
    EpochMismatch,
    InvalidEvidence,
    MissingEvidence,
    AmbiguousProgress,
    InconsistentProgress,
    FrameDiscontinuity,
    ContentMismatch,
    PresentationMismatch,
    RecoveryRequired,
};

template <typename T>
using Decision = std::expected<T, Rejection>;

struct AdvanceBounds {
    uint64_t minimum;
    uint64_t maximum;
};

// Bounds are inclusive advances since the previous *verified* position. A
// modulo pointer alone, or an unqualified nearest-lap estimate, is not evidence.
// No cycle-to-packet equality is assumed here: self-linked skips can consume
// cycles without advancing a descriptor (Linux ohci.c:3250-3256).
[[nodiscard]] constexpr Decision<uint64_t> ResolveProgress(
    uint64_t previous, uint32_t slot, uint32_t ringPackets,
    std::optional<AdvanceBounds> bounds) noexcept {
    if (!bounds) return std::unexpected(Rejection::MissingEvidence);
    if (ringPackets == 0 || slot >= ringPackets ||
        bounds->minimum > bounds->maximum ||
        bounds->maximum > std::numeric_limits<uint64_t>::max() - previous) {
        return std::unexpected(Rejection::InvalidEvidence);
    }
    const uint64_t low = previous + bounds->minimum;
    const uint64_t high = previous + bounds->maximum;
    const uint64_t offset = (uint64_t{slot} + ringPackets - low % ringPackets)
                          % ringPackets;
    if (offset > high - low) {
        return std::unexpected(Rejection::InconsistentProgress);
    }
    const uint64_t first = low + offset; // <= high, so cannot overflow.
    if (high - first >= ringPackets) {
        return std::unexpected(Rejection::AmbiguousProgress);
    }
    return first;
}

struct EpochAgreement {
    uint64_t rx;
    uint64_t tx;
    uint64_t pcm;
};

// The expected identity and the first frame's presentation interval come from
// outside HardwareSampleTimeline. The endpoints include all independently
// justified clock/correlation uncertainty. There is no default tolerance and
// no extrapolation from a permanent nominal-rate startup anchor.
struct PresentationEvidence {
    uint64_t epoch;
    uint64_t firstAudioFrame;
    uint32_t frameCount;
    uint64_t earliestBusTicks;
    uint64_t latestBusTicks;
};

// Single-threaded verification state. Only a successful Decision contains a
// range eligible for timing aggregates. A current-epoch failure blocks further
// admission until a strictly newer, coherent epoch is established. Replayed
// records wholly from an old epoch are rejected without poisoning the new one.
class Contract final {
public:
    [[nodiscard]] Decision<void> BeginEpoch(
        EpochAgreement epochs, uint64_t firstAudioFrame,
        uint64_t verifiedPacketIndex) noexcept {
        if (epochs.rx == 0 || epochs.rx != epochs.tx || epochs.rx != epochs.pcm ||
            epochs.rx <= epoch_) {
            blocked_ = true;
            return std::unexpected(Rejection::EpochMismatch);
        }
        epoch_ = epochs.rx;
        nextFrame_ = firstAudioFrame;
        packetIndex_ = verifiedPacketIndex;
        blocked_ = false;
        progressVerified_ = false;
        ringPackets_ = 0;
        return {};
    }

    [[nodiscard]] Decision<uint64_t> ObserveProgress(
        EpochAgreement epochs, uint32_t slot, uint32_t ringPackets,
        std::optional<AdvanceBounds> bounds) noexcept {
        // No record accompanies a progress observation, so a stale snapshot is
        // an entirely old call by definition.
        if (auto check = CheckEpoch(epochs, std::nullopt); !check) {
            return std::unexpected(check.error());
        }
        if (ringPackets_ != 0 && ringPackets != ringPackets_) {
            return Fail<uint64_t>(Rejection::InvalidEvidence);
        }
        const auto resolved = ResolveProgress(packetIndex_, slot, ringPackets, bounds);
        if (!resolved) return Fail<uint64_t>(resolved.error());
        packetIndex_ = *resolved;
        ringPackets_ = ringPackets;
        progressVerified_ = true;
        return packetIndex_;
    }

    [[nodiscard]] Decision<Audio::Runtime::TxPresentationRange> Admit(
        EpochAgreement epochs, const Audio::Runtime::TxPresentationRange& range,
        std::optional<PresentationEvidence> evidence) noexcept {
        // A record's age cannot be judged until its own stamps agree. A range
        // and evidence naming different epochs is a broken handoff whatever the
        // snapshot says.
        if (evidence && evidence->epoch != range.epoch) {
            return Fail<Audio::Runtime::TxPresentationRange>(Rejection::EpochMismatch);
        }
        if (auto check = CheckEpoch(epochs, range.epoch); !check) {
            return std::unexpected(check.error());
        }
        if (range.epoch != epoch_) {
            // Only an entirely old record is harmless. Mixing an old range
            // with current-epoch state is a broken recovery handoff.
            return Fail<Audio::Runtime::TxPresentationRange>(Rejection::EpochMismatch);
        }
        if (!progressVerified_ || !evidence) {
            return Fail<Audio::Runtime::TxPresentationRange>(Rejection::MissingEvidence);
        }
        // evidence->epoch == range.epoch == epoch_ is established above.
        constexpr auto max = std::numeric_limits<uint64_t>::max();
        if (range.frameCount == 0 || evidence->frameCount == 0 ||
            range.frameCount > max - range.firstAudioFrame ||
            evidence->frameCount > max - evidence->firstAudioFrame ||
            evidence->earliestBusTicks > evidence->latestBusTicks) {
            return Fail<Audio::Runtime::TxPresentationRange>(Rejection::InvalidEvidence);
        }
        if (range.firstAudioFrame != nextFrame_) {
            return Fail<Audio::Runtime::TxPresentationRange>(Rejection::FrameDiscontinuity);
        }
        if (range.firstAudioFrame != evidence->firstAudioFrame ||
            range.frameCount != evidence->frameCount) {
            return Fail<Audio::Runtime::TxPresentationRange>(Rejection::ContentMismatch);
        }
        if (range.presentationBusTicks < evidence->earliestBusTicks ||
            range.presentationBusTicks > evidence->latestBusTicks) {
            return Fail<Audio::Runtime::TxPresentationRange>(Rejection::PresentationMismatch);
        }
        nextFrame_ = range.firstAudioFrame + range.frameCount;
        return range;
    }

    [[nodiscard]] uint64_t NextFrame() const noexcept { return nextFrame_; }
    [[nodiscard]] uint64_t PacketIndex() const noexcept { return packetIndex_; }

private:
    template <typename T>
    [[nodiscard]] Decision<T> Fail(Rejection rejection) noexcept {
        blocked_ = true;
        return std::unexpected(rejection);
    }

    // `recordEpoch` is the epoch stamped on the record this call carries, or
    // nullopt when it carries none. A record is *entirely* old -- and so
    // harmless to a newer epoch -- only when the snapshot and the record name
    // the same older epoch. A stale snapshot paired with a current-epoch record
    // is the same broken handoff as its mirror (current snapshot, old range),
    // and must demand recovery rather than be waved through as merely late.
    [[nodiscard]] Decision<void> CheckEpoch(
        EpochAgreement epochs, std::optional<uint64_t> recordEpoch) noexcept {
        if (epoch_ == 0) return std::unexpected(Rejection::NoEpoch);
        const bool coherentStaleSnapshot =
            epochs.rx != 0 && epochs.rx == epochs.tx && epochs.rx == epochs.pcm &&
            epochs.rx < epoch_;
        if (coherentStaleSnapshot && (!recordEpoch || *recordEpoch == epochs.rx)) {
            return std::unexpected(Rejection::StaleEpoch);
        }
        if (epochs.rx != epoch_ || epochs.tx != epoch_ || epochs.pcm != epoch_) {
            return Fail<void>(Rejection::EpochMismatch);
        }
        if (blocked_) return std::unexpected(Rejection::RecoveryRequired);
        return {};
    }

    uint64_t epoch_{0};
    uint64_t nextFrame_{0};
    uint64_t packetIndex_{0};
    uint32_t ringPackets_{0};
    bool blocked_{false};
    bool progressVerified_{false};
};

} // namespace ASFW::Testing::AudioTiming
