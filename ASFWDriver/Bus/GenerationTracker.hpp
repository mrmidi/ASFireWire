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

    /// Apply the generation confirmed from the OHCI SelfIDCount register.
    /// This is NOT the AR bus-reset marker generation — that value is informational
    /// only and is never fed into the tracker.
    void OnConfirmedBusGeneration(uint8_t confirmedGeneration) noexcept;

    void OnSelfIDComplete(uint16_t newNodeID) noexcept;

    void Reset() noexcept;

private:
    void ApplyBusGeneration(uint8_t generation8bit, const char* source) noexcept;

    ASFW::Async::LabelAllocator& labelAllocator_;

    std::atomic<uint8_t> busGeneration8bit_{0};
    std::atomic<uint16_t> localNodeID_{0};
};

} // namespace ASFW::Async::Bus
