
#pragma once

#include <atomic>
#include <optional>
#include "../../Common/BarrierUtils.hpp"
#include "../../Shared/Memory/IDMAMemory.hpp"

namespace ASFW::Isoch::Memory {

// Interface for Isochronous DMA Memory Management
// Extends generic IDMAMemory with Isoch-specific sub-allocation APIs.
class IIsochDMAMemory : public Shared::IDMAMemory {
public:
    virtual ~IIsochDMAMemory() = default;

    // Explicitly allocate a descriptor region (from descriptor slab)
    virtual std::optional<Shared::DMARegion> AllocateDescriptor(size_t size) = 0;

    // Explicitly allocate a payload buffer region (from payload slab)
    virtual std::optional<Shared::DMARegion> AllocatePayloadBuffer(size_t size) = 0;

    /// Orders payload publication before descriptor publication. Kept virtual
    /// so host tests can record the exact DMA visibility sequence.
    virtual void PublishBarrier() const noexcept {
        std::atomic_thread_fence(std::memory_order_release);
        ASFW::Driver::IoBarrier();
    }
};

} // namespace ASFW::Isoch::Memory
