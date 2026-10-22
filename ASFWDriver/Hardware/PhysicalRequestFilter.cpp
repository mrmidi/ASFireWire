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
    // reset may have handed to another device. Linux checks under the lock its
    // reset work also takes; we have no such lock against the interrupt path,
    // so the check is repeated after the write and the bit taken back if a
    // reset slipped in between.
    const auto stale = [&access, generation]() {
        const uint32_t current = (access.Read(Register32::kSelfIDCount) &
                                  SelfIDCountBits::kGenerationMask) >> SelfIDCountBits::kGenerationShift;
        return current != (generation & 0xFFu) ||
               (access.Read(Register32::kIntEvent) & IntEventBits::kBusReset) != 0;
    };
    if (stale()) {
        return std::unexpected(PhysicalDmaError::StaleGeneration);
    }
    const Register32 set = phy < 32 ? Register32::kPhyReqFilterLoSet : Register32::kPhyReqFilterHiSet;
    const Register32 clear = phy < 32 ? Register32::kPhyReqFilterLoClear : Register32::kPhyReqFilterHiClear;
    const uint32_t bit = 1u << (phy % 32);
    access.WriteAndFlush(set, bit);
    if (stale()) {
        access.WriteAndFlush(clear, bit);
        return std::unexpected(PhysicalDmaError::StaleGeneration);
    }
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
