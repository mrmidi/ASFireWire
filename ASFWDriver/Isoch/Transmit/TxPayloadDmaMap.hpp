#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace ASFW::Isoch::Tx {

struct TxPayloadDmaSegment {
    std::uint64_t deviceAddress{0};
    std::uint64_t length{0};
};

struct TxPayloadDmaFragment {
    std::uint32_t deviceAddress{0};
    std::uint32_t length{0};
};

class TxPayloadDmaMap final {
public:
    // DriverKit's PrepareForDMA RPC serializes IOAddressSegment segments[32].
    static constexpr std::size_t kMaxSegments = 32;

    struct MappedSegment {
        std::uint64_t slabOffset{0};
        std::uint64_t deviceAddress{0};
        std::uint64_t length{0};
    };

    [[nodiscard]] bool Configure(std::span<const TxPayloadDmaSegment> segments,
                                 std::uint64_t slabLength) noexcept {
        Reset();
        if (segments.empty() || segments.size() > kMaxSegments || slabLength == 0) {
            return false;
        }

        std::uint64_t remaining = slabLength;
        std::uint64_t slabOffset = 0;

        for (const auto& segment : segments) {
            if (remaining == 0) {
                break;
            }
            if (segment.deviceAddress == 0 || segment.length == 0) {
                Reset();
                return false;
            }

            const auto mappedLength = segment.length < remaining ? segment.length : remaining;
            constexpr auto kAddressSpaceSize =
                std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1ULL;
            if (segment.deviceAddress >= kAddressSpaceSize ||
                mappedLength > (kAddressSpaceSize - segment.deviceAddress)) {
                Reset();
                return false;
            }

            segments_[segmentCount_++] = MappedSegment{
                .slabOffset = slabOffset,
                .deviceAddress = segment.deviceAddress,
                .length = mappedLength,
            };
            slabOffset += mappedLength;
            remaining -= mappedLength;
        }

        if (remaining != 0) {
            Reset();
            return false;
        }

        slabLength_ = slabLength;
        return true;
    }

    void Reset() noexcept {
        segments_ = {};
        segmentCount_ = 0;
        slabLength_ = 0;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return segmentCount_ != 0 && slabLength_ != 0;
    }

    [[nodiscard]] std::size_t SegmentCount() const noexcept {
        return segmentCount_;
    }

    [[nodiscard]] std::uint64_t SlabLength() const noexcept {
        return slabLength_;
    }

    [[nodiscard]] const MappedSegment* SegmentAt(std::size_t index) const noexcept {
        return index < segmentCount_ ? &segments_[index] : nullptr;
    }

    /// Split a payload across the two descriptor entries an isoch packet
    /// program carries.
    ///
    /// `prefixLength` is an opaque count of leading bytes the producer says are
    /// identical in every image of this payload. When it is usable, the split
    /// lands exactly there, so the second entry addresses only the bytes that
    /// can differ -- and re-pointing the payload at another image becomes a
    /// single aligned store. When it is not (zero, or the whole payload), the
    /// split is arbitrary and the halving below is as good as anything.
    [[nodiscard]] bool ResolveTwoFragments(
        std::uint64_t slabOffset,
        std::uint32_t length,
        std::array<TxPayloadDmaFragment, 2>& fragments,
        std::uint32_t prefixLength = 0) const noexcept {
        fragments = {};
        if (length == 0) {
            return true;
        }
        if (!IsValid() || length < 2 || slabOffset >= slabLength_ ||
            std::uint64_t{length} > (slabLength_ - slabOffset)) {
            return false;
        }

        std::uint64_t currentOffset = slabOffset;
        std::uint32_t remaining = length;
        std::size_t fragmentCount = 0;

        while (remaining != 0) {
            const MappedSegment* mapped = nullptr;
            for (std::size_t i = 0; i < segmentCount_; ++i) {
                const auto& candidate = segments_[i];
                if (currentOffset >= candidate.slabOffset &&
                    currentOffset < candidate.slabOffset + candidate.length) {
                    mapped = &candidate;
                    break;
                }
            }
            if (!mapped || fragmentCount == fragments.size()) {
                return false;
            }

            const auto segmentOffset = currentOffset - mapped->slabOffset;
            const auto available = mapped->length - segmentOffset;
            const auto fragmentLength =
                std::min<std::uint64_t>(available, remaining);
            fragments[fragmentCount++] = TxPayloadDmaFragment{
                .deviceAddress = static_cast<std::uint32_t>(
                    mapped->deviceAddress + segmentOffset),
                .length = static_cast<std::uint32_t>(fragmentLength),
            };
            currentOffset += fragmentLength;
            remaining -= static_cast<std::uint32_t>(fragmentLength);
        }

        if (fragmentCount == 1) {
            // Both entries must carry bytes, so a prefix that would leave the
            // second empty is not usable.
            const bool prefixUsable =
                prefixLength != 0 && prefixLength < fragments[0].length;
            const auto firstLength = prefixUsable
                ? prefixLength : (fragments[0].length / 2);
            const auto secondLength = fragments[0].length - firstLength;
            fragments[1] = TxPayloadDmaFragment{
                .deviceAddress = fragments[0].deviceAddress + firstLength,
                .length = secondLength,
            };
            fragments[0].length = firstLength;
        }

        return fragments[0].length != 0 && fragments[1].length != 0;
    }

private:
    std::array<MappedSegment, kMaxSegments> segments_{};
    std::size_t segmentCount_{0};
    std::uint64_t slabLength_{0};
};

} // namespace ASFW::Isoch::Tx
