#pragma once

#include <cstdint>

namespace ASFW::Isoch::Core {

enum class IsochEventDirection : uint8_t {
    kReceive,
    kTransmit,
};

struct IsochEventGroup final {
    IsochEventDirection direction{IsochEventDirection::kTransmit};
    uint64_t hostTicks{0};
    uint32_t hwPacketIndex{0};
    uint32_t completedPacketIndex{0};
    uint32_t completedPacketCount{0};
    uint32_t firstRefillPacket{0};
    uint32_t refillPacketCount{0};
    uint16_t outputLastTimestamp{0};
    uint64_t sampleFrame{0};

    [[nodiscard]] bool HasCompletionTimestamp() const noexcept {
        return outputLastTimestamp != 0;
    }
};

// TODO(DICE): Unify timing group packet count (currently 8) with Transmit Layout (kTimingGroupPackets) in a single place
[[nodiscard]] constexpr uint32_t TimingGroupPacketCount48k() noexcept {
    return 8;
}

[[nodiscard]] constexpr bool IsTimingGroupBoundary(uint32_t packetIndex) noexcept {
    return (packetIndex % TimingGroupPacketCount48k()) == (TimingGroupPacketCount48k() - 1);
}

[[nodiscard]] constexpr uint32_t PreviousPacketIndex(uint32_t packetIndex,
                                                     uint32_t ringPackets) noexcept {
    return ringPackets == 0 ? 0 : (packetIndex + ringPackets - 1) % ringPackets;
}

} // namespace ASFW::Isoch::Core
