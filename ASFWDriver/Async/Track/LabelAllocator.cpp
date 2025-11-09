#include "LabelAllocator.hpp"

#include <bit>
#include "../../Core/FWCommon.hpp"

namespace ASFW::Async {

LabelAllocator::LabelAllocator()
    : bitmap_(0), generation_(0), next_label_(0) {}

void LabelAllocator::Reset() {
    bitmap_.store(0, std::memory_order_relaxed);
    generation_.store(0, std::memory_order_relaxed);
    next_label_.store(0, std::memory_order_relaxed);
}

uint8_t LabelAllocator::Allocate() {
    uint64_t current = bitmap_.load(std::memory_order_relaxed);

    while (true) {
        uint64_t available = ~current;
        if (available == 0) {
            return kInvalidLabel;
        }

        unsigned int index = static_cast<unsigned int>(std::countr_zero(available));
        if (index >= kMaxLabels) {
            return kInvalidLabel;
        }

        uint64_t desired = current | ASFW::FW::bit<uint64_t>(index);
        if (bitmap_.compare_exchange_weak(current,
                                          desired,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            return static_cast<uint8_t>(index);
        }
        // compare_exchange_weak updated `current`; loop retries with fresh snapshot.
    }
}

uint8_t LabelAllocator::NextLabel() noexcept {
    return next_label_.fetch_add(1, std::memory_order_relaxed) & 0x3F;
}

void LabelAllocator::Free(uint8_t label) {
    if (label >= kMaxLabels) {
        return;
    }
    const uint64_t mask = ASFW::FW::bit<uint64_t>(label);
    bitmap_.fetch_and(~mask, std::memory_order_release);
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

