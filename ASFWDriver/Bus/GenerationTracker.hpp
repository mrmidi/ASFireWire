#pragma once

#include <atomic>
#include <cstdint>

#include "../Async/Track/LabelAllocator.hpp"

namespace ASFW::Async::Bus {

class GenerationTracker {
public:
    struct BusState {
        uint16_t generation16; // logical (extended) generation
        uint8_t generation8;   // raw OHCI 8-bit generation for packet headers
        uint16_t localNodeID;  // 0 == unknown
    };

    explicit GenerationTracker(ASFW::Async::LabelAllocator& allocator) noexcept;

    [[nodiscard]] BusState GetCurrentState() const noexcept;

    // Called from AR receive path when a synthetic bus-reset packet is observed.
    // MUST be noexcept and lock-free: no allocations, no locks.
    void OnSyntheticBusReset(uint8_t newGenerationFromPacket) noexcept;

    // Called after Self-ID completes and NodeID register is valid.
    void OnSelfIDComplete(uint16_t newNodeID) noexcept;

    // Reset to initial state. Called from Start/Teardown.
    void Reset() noexcept;

private:
    void ApplyBusGeneration(uint8_t generation8bit, const char* source) noexcept;

    ASFW::Async::LabelAllocator& labelAllocator_;

    std::atomic<uint8_t> busGeneration8bit_{0};
    std::atomic<uint16_t> localNodeID_{0};
};

} // namespace ASFW::Async::Bus
