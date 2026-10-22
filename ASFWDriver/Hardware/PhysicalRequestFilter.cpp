#include "HardwareInterface.hpp"
#include "OHCIConstants.hpp"
#include "RegisterMap.hpp"
#include "../Logging/Logging.hpp"

namespace ASFW::Driver {

std::expected<void, PhysicalDmaError>
HardwareInterface::EnablePhysicalRequests(uint16_t node, uint16_t generation) noexcept {
    const uint16_t bus = node & 0xFFC0u;
    const uint16_t phy = node & 0x3Fu;
    if ((bus != 0 && bus != 0xFFC0u) || phy == 63) {
        return std::unexpected(PhysicalDmaError::InvalidNode);
    }
    auto access = TryBeginAccess();
    if (!access) {
        return std::unexpected(PhysicalDmaError::HardwareGone);
    }
    // Linux ohci.c:2615: a stale generation must not arm a node number the
    // reset may have handed to another device.
    const uint32_t current = (access.Read(Register32::kSelfIDCount) &
                              SelfIDCountBits::kGenerationMask) >> SelfIDCountBits::kGenerationShift;
    if (current != (generation & 0xFFu) ||
        (access.Read(Register32::kIntEvent) & IntEventBits::kBusReset) != 0) {
        return std::unexpected(PhysicalDmaError::StaleGeneration);
    }
    access.WriteAndFlush(phy < 32 ? Register32::kPhyReqFilterLoSet : Register32::kPhyReqFilterHiSet,
                         1u << (phy % 32));
    ASFW_LOG(Hardware, "[PhysicalDMA] node=%u gen=%u enabled", phy, generation);
    return {};
}

void HardwareInterface::ClearPhysicalRequestFilters() noexcept {
    if (auto access = TryBeginAccess()) {
        access.Write(Register32::kPhyReqFilterHiClear, 0xFFFFFFFFu);
        access.WriteAndFlush(Register32::kPhyReqFilterLoClear, 0xFFFFFFFFu);
    }
}

} // namespace ASFW::Driver
