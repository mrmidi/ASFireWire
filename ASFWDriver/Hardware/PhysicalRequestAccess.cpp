#include "HardwareInterface.hpp"
#include "OHCIConstants.hpp"
#include "../Logging/Logging.hpp"

namespace ASFW::Driver {

bool HardwareInterface::ControlPhysicalRequests(uint16_t node, uint16_t generation,
                                               PhysicalRequestAccess action) noexcept {
    // Accept short IDs and local-bus qualified IDs, never broadcast or remote buses.
    const uint16_t bus = node & 0xFFC0u;
    if ((bus != 0 && bus != 0xFFC0u) || (node & 0x3Fu) == 63) return false;
    node &= 0x3Fu;
    auto access = TryBeginAccess();
    if (!access) return false;

    const bool enable = action == PhysicalRequestAccess::Acquire;
    const bool check = action == PhysicalRequestAccess::Check;
    if (enable || check) {
        // Linux ohci.c:2615 rejects a stale generation before touching the filter.
        const uint32_t count = access.Read(Register32::kSelfIDCount);
        const uint32_t current = (count & SelfIDCountBits::kGenerationMask) >>
                                 SelfIDCountBits::kGenerationShift;
        if (generation != current ||
            (access.Read(Register32::kIntEvent) & IntEventBits::kBusReset) != 0) return false;
        if (physicalRequestGeneration_ != generation) {
            physicalRequestUsers_.fill(0);
            physicalRequestBlocked_ = 0;
            physicalRequestGeneration_ = generation;
        }
    } else if (physicalRequestGeneration_ != generation) {
        return true;
    }

    const uint64_t nodeMask = uint64_t{1} << node;
    const bool hi = node >= 32;
    const uint32_t bit = 1u << (node % 32);
    const Register32 set = hi ? Register32::kPhyReqFilterHiSet : Register32::kPhyReqFilterLoSet;
    const Register32 clear = hi ? Register32::kPhyReqFilterHiClear : Register32::kPhyReqFilterLoClear;
    auto& users = physicalRequestUsers_[node];
    if ((enable || check) && (physicalRequestBlocked_ & nodeMask) != 0) return false;
    if (check) return users != 0 && (access.Read(set) & bit) != 0;

    if (enable) {
        if (users != 0) {
            ++users;
            return true;
        }
        access.Write(Register32::kPhyUpperBound, kPhysicalUpperBound);
        access.WriteAndFlush(set, bit);
    } else {
        if (action == PhysicalRequestAccess::Revoke) {
            // An unconfirmed abort fences every LUN on this node until bus reset.
            physicalRequestBlocked_ |= nodeMask;
            users = 0;
        } else if (users == 0 || --users != 0) {
            return true;
        }
        access.WriteAndFlush(clear, bit);
    }

    const uint32_t readback = access.Read(set);
    bool applied = ((readback & bit) != 0) == enable;
    if (enable) {
        const uint32_t count = access.Read(Register32::kSelfIDCount);
        const uint32_t current = (count & SelfIDCountBits::kGenerationMask) >>
                                 SelfIDCountBits::kGenerationShift;
        if (generation != current ||
            (access.Read(Register32::kIntEvent) & IntEventBits::kBusReset) != 0) {
            access.WriteAndFlush(clear, bit);
            applied = false;
        }
    }
    if (enable && applied) users = 1;
    ASFW_LOG(Hardware,
             "[PhysicalDMA] node=%u gen=%u enable=%u filter=0x%08x applied=%u",
             node, generation, enable ? 1u : 0u, readback, applied ? 1u : 0u);
    return applied;
}

void HardwareInterface::ClearPhysicalRequestFilters() noexcept {
    auto access = TryBeginAccess();
    if (access) ClearPhysicalRequestFilters(access);
}

void HardwareInterface::ClearPhysicalRequestFilters(HardwareAccessScope& access) noexcept {
    access.Write(Register32::kPhyReqFilterHiClear, 0xFFFFFFFFu);
    access.WriteAndFlush(Register32::kPhyReqFilterLoClear, 0xFFFFFFFFu);
    physicalRequestUsers_.fill(0);
    physicalRequestBlocked_ = 0;
    physicalRequestGeneration_ = 0xFFFF;
    const uint32_t hi = access.Read(Register32::kPhyReqFilterHiSet);
    const uint32_t lo = access.Read(Register32::kPhyReqFilterLoSet);
    ASFW_LOG(Hardware, "[PhysicalDMA] clear hi=0x%08x lo=0x%08x", hi, lo);
}

} // namespace ASFW::Driver
