#include "LabelAllocator.hpp"

#include <bit>
#include "../../Common/FWCommon.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Logging/LogConfig.hpp"

namespace ASFW::Async {

LabelAllocator::LabelAllocator()
    : bitmap_(0), generation_(0), next_label_(0) {}

void LabelAllocator::Reset() {
    bitmap_.store(0, std::memory_order_relaxed);
    generation_.store(0, std::memory_order_relaxed);
    next_label_.store(0, std::memory_order_relaxed);
}

uint8_t LabelAllocator::Allocate() {
    // Round-robin allocator: start from next_label_ cursor, scan for a free bit.
    uint8_t start = next_label_.load(std::memory_order_relaxed);
    uint64_t snapshot = bitmap_.load(std::memory_order_relaxed);

    for (unsigned int attempt = 0; attempt < kMaxLabels; ++attempt) {
        const uint8_t idx = static_cast<uint8_t>((start + attempt) & 0x3F);
        const uint64_t mask = ASFW::FW::bit<uint64_t>(idx);
        if (snapshot & mask) {
            continue;  // in use
        }

        const uint64_t desired = snapshot | mask;
        if (bitmap_.compare_exchange_weak(snapshot,
                                          desired,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            const uint8_t next = static_cast<uint8_t>((idx + 1) & 0x3F);
            next_label_.store(next, std::memory_order_relaxed);
            ASFW_LOG_V3(Async, "LabelAllocator::Allocate: label=%u bitmap=0x%016llx→0x%016llx next=%u",
                        idx, snapshot, desired, next);
            return idx;
        }
        // CAS failed; snapshot updated with current bitmap, retry loop.
    }

    ASFW_LOG_V0(Async, "LabelAllocator::Allocate: no free labels (bitmap=0x%016llx)", snapshot);
    return kInvalidLabel;
}

uint8_t LabelAllocator::NextLabel() noexcept {
    // IEEE-1394 tLabel is 6-bit (0-63), must wrap properly.
    // Use compare-exchange to ensure both returned value AND stored counter wrap at 63→0.
    uint8_t current = next_label_.load(std::memory_order_relaxed);
    uint8_t next;

    do {
        next = (current + 1) & 0x3F;  // Wrap at 64 (6-bit field)
    } while (!next_label_.compare_exchange_weak(
        current,
        next,
        std::memory_order_relaxed,
        std::memory_order_relaxed
    ));

    return current;  // Return the label we reserved (before increment)
}

void LabelAllocator::Free(uint8_t label) {
    if (label >= kMaxLabels) {
        return;
    }
    const uint64_t mask = ASFW::FW::bit<uint64_t>(label);
    const uint64_t before = bitmap_.fetch_and(~mask, std::memory_order_release);
    ASFW_LOG_V3(Async, "LabelAllocator::Free: label=%u bitmap=0x%016llx→0x%016llx",
                label,
                before,
                before & ~mask);
}

void LabelAllocator::ClearBitmap() {
    const uint64_t before = bitmap_.exchange(0, std::memory_order_release);
    next_label_.store(0, std::memory_order_relaxed);
    ASFW_LOG(Async, "LabelAllocator::ClearBitmap: bitmap=0x%016llx→0x0000000000000000", before);
}

void LabelAllocator::BumpGeneration() {
    uint16_t current = generation_.load(std::memory_order_relaxed);
    while (!generation_.compare_exchange_weak(current,
                                              static_cast<uint16_t>((current + 1) & kGenerationMask),
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
        // retry until generation is updated atomically
    }
}

void LabelAllocator::SetGeneration(uint16_t newGen) {
    generation_.store(newGen & kGenerationMask, std::memory_order_release);
}

uint16_t LabelAllocator::CurrentGeneration() const {
    return generation_.load(std::memory_order_acquire) & kGenerationMask;
}

bool LabelAllocator::IsLabelInUse(uint8_t label) const {
    if (label >= kMaxLabels) {
        return false;
    }
    const uint64_t mask = ASFW::FW::bit<uint64_t>(label);
    return (bitmap_.load(std::memory_order_acquire) & mask) != 0;
}

} // namespace ASFW::Async
