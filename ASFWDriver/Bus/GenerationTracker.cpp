#include "GenerationTracker.hpp"
#include "../Logging/Logging.hpp"

namespace ASFW::Async::Bus {

using namespace ASFW::Async;

GenerationTracker::GenerationTracker(LabelAllocator& allocator) noexcept
    : labelAllocator_(allocator)
{}

void GenerationTracker::Reset() noexcept {
    localNodeID_.store(0, std::memory_order_release);
    busGeneration8bit_.store(0, std::memory_order_release);
    labelAllocator_.Reset();
}

GenerationTracker::BusState GenerationTracker::GetCurrentState() const noexcept {
    const uint16_t gen16 = labelAllocator_.CurrentGeneration();
    const uint16_t node = localNodeID_.load(std::memory_order_acquire);
    const uint8_t gen8 = static_cast<uint8_t>(gen16 & 0x00FF);
    return BusState{ .generation16 = gen16, .generation8 = gen8, .localNodeID = node };
}

void GenerationTracker::OnSyntheticBusReset(uint8_t newGenerationFromPacket) noexcept {
    ASFW_LOG(Async, "GenerationTracker: Synthetic bus reset detected. New generation: %u", newGenerationFromPacket);
    localNodeID_.store(0, std::memory_order_release);
    ApplyBusGeneration(newGenerationFromPacket, "synthetic-packet");
}

void GenerationTracker::OnSelfIDComplete(uint16_t newNodeID) noexcept {
    ASFW_LOG(Async, "GenerationTracker: Self-ID complete. New NodeID: 0x%04x", newNodeID);
    localNodeID_.store(newNodeID, std::memory_order_release);
}

void GenerationTracker::ApplyBusGeneration(uint8_t generation8bit, const char* source) noexcept {
    const uint8_t previous8bit = busGeneration8bit_.exchange(generation8bit, std::memory_order_acq_rel);
    const uint16_t current16bit = labelAllocator_.CurrentGeneration();
    const uint8_t currentLow8bit = static_cast<uint8_t>(current16bit & 0x00FF);
    uint16_t newHigh = static_cast<uint16_t>(current16bit & 0xFF00);

    if (generation8bit < currentLow8bit) {
        newHigh = static_cast<uint16_t>(newHigh + 0x0100);
    }

    const uint16_t newGen16 = static_cast<uint16_t>(newHigh | generation8bit);
    labelAllocator_.SetGeneration(newGen16);

    ASFW_LOG(Async,
             "Bus generation update (%{public}s): prev8=%u, new8=%u -> prev16=0x%04x, new16=0x%04x",
             source,
             previous8bit,
             generation8bit,
             current16bit,
             newGen16);
}

} // namespace ASFW::Async::Bus
