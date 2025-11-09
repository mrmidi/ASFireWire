#pragma once

#include <cstdint>
#include <memory>

#pragma once

#include <cstdint>
#include <memory>

// Forward declarations
class IOMemoryMap;

namespace ASFW::Driver {
class HardwareInterface;
}

namespace ASFW::Async {

/**
 * PayloadContext - RAII wrapper for DMA-mapped payload buffers in async transactions.
 * 
 * OWNERSHIP MODEL:
 * ---------------
 * Factory returns unique_ptr<PayloadContext> (exclusive ownership).
 * Caller retains unique ownership until PayloadRegistry::Attach().
 * ToSharedPtr() converts to shared_ptr<void> with custom deleter:
 *   [](void* p){ delete static_cast<PayloadContext*>(p); }
 * enabling RAII semantics across shared ownership boundary.
 * 
 * Destructor guarantees DMA resource cleanup on scope exit or shared_ptr destruction.
 * No manual Cleanup() calls required - RAII handles lifecycle automatically.
 * 
 * LIFECYCLE:
 * ---------
 * 1. Create() allocates DMABuffer and maps to bus-addressable memory
 * 2. Caller holds unique_ptr during descriptor chain construction
 * 3. DeviceAddress() provides bus address for descriptor.dataAddress field
 * 4. ToSharedPtr() converts for PayloadRegistry tracking
 * 5. Destructor unmaps IOMemoryMap and releases DMABuffer when refcount→0
 * 
 * Reference: Apple's IOFWAsyncCommand allocates per-transaction IODMACommand
 *            and keeps it alive until completion (see DECOMPILATION.md §Async Submission).
 */
class PayloadContext {
public:
    /**
     * Factory method - creates DMA-mapped payload buffer.
     * 
     * @param hw Hardware interface for DMA allocation
     * @param data Source data to copy into DMA buffer
     * @param length Payload size in bytes
     * @param direction IOMemoryDirectionIn (device→host) or Out (host→device)
     * @return unique_ptr<PayloadContext> on success, nullptr on allocation/mapping failure
     */
    static std::unique_ptr<PayloadContext> Create(
        ASFW::Driver::HardwareInterface& hw,
        const void* data,
        std::size_t length,
        uint64_t direction);
    
    /**
     * Destructor - automatically cleans up DMA resources (RAII).
     * Unmaps IOMemoryMap and releases HardwareInterface DMABuffer.
     */
    ~PayloadContext();
    
    // Non-copyable, non-movable (owns DMA resources)
    PayloadContext(const PayloadContext&) = delete;
    PayloadContext& operator=(const PayloadContext&) = delete;
    PayloadContext(PayloadContext&&) = delete;
    PayloadContext& operator=(PayloadContext&&) = delete;
    
    /**
     * Get bus-visible physical address for OHCI descriptor.dataAddress field.
     * @return 32-bit physical address (guaranteed <4GB per OHCI 1.1 spec), or 0 if unmapped
     */
    [[nodiscard]] uint64_t DeviceAddress() const noexcept;
    
    /**
     * Convert unique_ptr to shared_ptr for PayloadRegistry attachment.
     * Consumes the unique_ptr and transfers ownership to shared_ptr with custom deleter.
     * @param up unique_ptr to consume (will be moved)
     * @return shared_ptr managing the PayloadContext with type-correct deleter
     */
    static std::shared_ptr<void> IntoShared(std::unique_ptr<PayloadContext>&& up);

    [[nodiscard]] std::size_t Length() const noexcept { return length_; }

private:
    // Private constructor - use Create() factory
    PayloadContext() = default;
    
    /**
     * Internal initialization logic (called by Create()).
     * @return true on success, false on allocation/mapping failure
     */
    bool Initialize(ASFW::Driver::HardwareInterface& hw,
                   const void* data,
                   std::size_t length,
                   uint64_t direction);
    
    /**
     * Internal cleanup - unmaps IOMemoryMap and releases DMABuffer.
     * Called automatically by destructor (RAII).
     */
    void Cleanup();
    
    // DMA resource handles - use opaque pointer to avoid including HardwareInterface.hpp
    struct DMABufferImpl;
    std::unique_ptr<DMABufferImpl> dmaBufferImpl_;
    IOMemoryMap* mapping_{nullptr};
    uint8_t* virtualAddress_{nullptr};
    const void* logicalAddress_{nullptr};  // Source data pointer (host virtual address)
    std::size_t length_{0};
    uint64_t deviceAddress_{0};
};

} // namespace ASFW::Async
