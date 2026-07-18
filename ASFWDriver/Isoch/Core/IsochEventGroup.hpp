#pragma once

#include "IsochDmaGeometry.hpp"

#include <cstdint>

namespace ASFW::Isoch::Core {

enum class IsochEventDirection : uint8_t {
    kReceive,
    kTransmit,
};

[[nodiscard]] constexpr uint32_t InterruptGroupPacketCount() noexcept {
    return IsochDmaGeometry::kPacketsPerInterrupt;
}

[[nodiscard]] constexpr bool IsTimingGroupBoundary(uint32_t packetIndex) noexcept {
    return (packetIndex % InterruptGroupPacketCount()) ==
           (InterruptGroupPacketCount() - 1);
}

[[nodiscard]] constexpr uint32_t PreviousPacketIndex(uint32_t packetIndex,
                                                     uint32_t ringPackets) noexcept {
    return ringPackets == 0 ? 0 : (packetIndex + ringPackets - 1) % ringPackets;
}

} // namespace ASFW::Isoch::Core
