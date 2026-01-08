#pragma once

#include <atomic>
#include <cstdint>

#include "../Async/Track/LabelAllocator.hpp"

namespace ASFW::Async::Bus {

class GenerationTracker {
public:
    struct BusState {
        uint16_t generation16;
        uint8_t generation8;
        uint16_t localNodeID;
    };

    explicit GenerationTracker(ASFW::Async::LabelAllocator& allocator) noexcept;

    [[nodiscard]] BusState GetCurrentState() const noexcept;

    void OnSyntheticBusReset(uint8_t newGenerationFromPacket) noexcept;

    void OnSelfIDComplete(uint16_t newNodeID) noexcept;

    void Reset() noexcept;

private:
    void ApplyBusGeneration(uint8_t generation8bit, const char* source) noexcept;

    ASFW::Async::LabelAllocator& labelAllocator_;

    std::atomic<uint8_t> busGeneration8bit_{0};
    std::atomic<uint16_t> localNodeID_{0};
};

} // namespace ASFW::Async::Bus
