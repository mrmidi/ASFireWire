// IsochTxDescriptorSlab.hpp
// ASFW - IT DMA descriptor slab + page-gap addressing helpers.

#pragma once

#include "IsochTxLayout.hpp"
#include "../Memory/IIsochDMAMemory.hpp"
#include "../../Shared/Memory/IDMAMemory.hpp"
#include "../../Hardware/OHCIDescriptors.hpp"
#include "../../Logging/Logging.hpp"

#include <cstdint>
#include <cstring>

namespace ASFW::Isoch::Tx {

/// Owns the dedicated IT descriptor DMA region and provides page-gap-safe
/// descriptor addressing (Linux firewire-ohci padding strategy). TX payloads
/// live exclusively in the externally shared producer slab.
class IsochTxDescriptorSlab final {
public:
    using OHCIDescriptor = Async::HW::OHCIDescriptor;

    IsochTxDescriptorSlab() noexcept = default;

    [[nodiscard]] kern_return_t AllocateAndInitialize(Memory::IIsochDMAMemory& dmaMemory) noexcept;

    [[nodiscard]] bool IsValid() const noexcept {
        return descRegion_.virtualBase != nullptr;
    }

    void DebugFillDescriptorSlab(uint8_t pattern) noexcept {
        if (!descRegion_.virtualBase || descRegion_.size == 0) return;
        std::memset(descRegion_.virtualBase, pattern, descRegion_.size);
    }

    [[nodiscard]] Shared::DMARegion DescriptorRegion() const noexcept { return descRegion_; }

    // -------------------------------------------------------------------------
    // Page-aware descriptor addressing
    // -------------------------------------------------------------------------

    [[nodiscard]] OHCIDescriptor* GetDescriptorPtr(uint32_t logicalIndex) noexcept;
    [[nodiscard]] const OHCIDescriptor* GetDescriptorPtr(uint32_t logicalIndex) const noexcept;
    [[nodiscard]] uint32_t GetDescriptorIOVA(uint32_t logicalIndex) const noexcept;
    [[nodiscard]] bool DecodeCmdAddrToLogicalIndex(uint32_t cmdAddr, uint32_t& outLogicalIndex) const noexcept;
    void ValidateDescriptorLayout() const noexcept;

#ifdef ASFW_HOST_TEST
    // Host-only: allow exercising pure address math without allocating DMA.
    void AttachDescriptorBaseForTest(uint32_t descBaseIOVA32) noexcept { testDescBaseIOVA32_ = descBaseIOVA32; }
#endif

private:
    Shared::DMARegion descRegion_{};

#ifdef ASFW_HOST_TEST
    uint32_t testDescBaseIOVA32_{0};
#endif
};

} // namespace ASFW::Isoch::Tx
