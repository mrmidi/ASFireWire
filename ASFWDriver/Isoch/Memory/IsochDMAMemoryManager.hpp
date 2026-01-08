#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "IIsochDMAMemory.hpp"
#include "../../Shared/Memory/DMAMemoryManager.hpp"
#include "../../Hardware/HardwareInterface.hpp"

namespace ASFW::Isoch::Memory {

struct IsochMemoryConfig {
    size_t numDescriptors = 0;          // ring length
    size_t packetSizeBytes = 0;         // per-packet buffer size (max)
    size_t descriptorAlignment = 16;    // OHCI needs >=16
    size_t payloadPageAlignment = 16384; // modern macOS default
};

// Dedicated DMA for Isoch: separate from Async slab.
// Internally uses two independent DMAMemoryManager slabs:
//  - descriptor slab: small, tight alignment
//  - payload slab: large, with cursor aligned so buffers start at payloadPageAlignment IOVA boundary
class IsochDMAMemoryManager final : public IIsochDMAMemory {
public:
    static std::shared_ptr<IsochDMAMemoryManager> Create(const IsochMemoryConfig& cfg);

    ~IsochDMAMemoryManager() override;

    // Allocate the two slabs using the same AllocateDMA path as Async.
    bool Initialize(ASFW::Driver::HardwareInterface& hw);

    // IIsochDMAMemory Implementation
    std::optional<ASFW::Shared::DMARegion> AllocateDescriptor(size_t size) override;
    std::optional<ASFW::Shared::DMARegion> AllocatePayloadBuffer(size_t size) override;

    // IDMAMemory Implementation
    std::optional<ASFW::Shared::DMARegion> AllocateRegion(size_t size, size_t alignment = 16) override;

    uint64_t VirtToIOVA(const void* virt) const noexcept override;
    void* IOVAToVirt(uint64_t iova) const noexcept override;

    void PublishToDevice(const void* address, size_t length) const noexcept override;
    void FetchFromDevice(const void* address, size_t length) const noexcept override;

    size_t TotalSize() const noexcept override;
    size_t AvailableSize() const noexcept override;

private:
    explicit IsochDMAMemoryManager(const IsochMemoryConfig& cfg);

    static bool IsPowerOf2(size_t v) noexcept;
    static size_t RoundUp(size_t v, size_t align) noexcept;

    bool ValidateConfig() const noexcept;

    IsochMemoryConfig cfg_{};

    ASFW::Shared::DMAMemoryManager descMgr_;
    ASFW::Shared::DMAMemoryManager payloadMgr_;

    bool initialized_{false};
};

} // namespace ASFW::Isoch::Memory
