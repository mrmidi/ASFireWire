#pragma once
#include "../Shared/Memory/IDMAMemory.hpp"
#include "../Shared/Memory/DMAMemoryManager.hpp"

namespace ASFW::Async {

// Import Shared type used by DMAMemoryImpl
using ASFW::Shared::DMAMemoryManager;
using ASFW::Shared::IDMAMemory;
using ASFW::Shared::DMARegion;

/**
 * @brief Concrete implementation of IDMAMemory using DMAMemoryManager.
 *
 * Thin adapter that delegates to the existing DMA memory manager.
 * Provides a simple interface for DMA memory allocation and coherency management.
 */
class DMAMemoryImpl final : public IDMAMemory {
public:
    explicit DMAMemoryImpl(DMAMemoryManager& mgr);

    std::optional<DMARegion> AllocateRegion(size_t size, size_t alignment) override;
    uint64_t VirtToIOVA(const void* virt) const noexcept override;
    void* IOVAToVirt(uint64_t iova) const noexcept override;
    void PublishToDevice(const void* address, size_t length) const noexcept override;
    void FetchFromDevice(const void* address, size_t length) const noexcept override;
    size_t TotalSize() const noexcept override;
    size_t AvailableSize() const noexcept override;

private:
    DMAMemoryManager& mgr_;
};

} // namespace ASFW::Async
