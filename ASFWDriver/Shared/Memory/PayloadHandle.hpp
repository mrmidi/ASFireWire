#pragma once

#include <cstdint>
#include <span>

#include <DriverKit/IOReturn.h>

namespace ASFW::Shared {

// Forward declarations
class DMAMemoryManager;
class PayloadHandle;  // Forward declare for PayloadPolicy

} // namespace ASFW::Shared

// Include PayloadPolicy after forward declaration (Phase 1.3)
#include "PayloadPolicy.hpp"

namespace ASFW::Shared {

/**
 * \brief RAII handle for DMA payload memory.
 *
 * Manages lifecycle of DMA-allocated payload buffers. Provides automatic
 * cleanup semantics compatible with Transaction ownership model.
 *
 * **Design**
 * - Movable but not copyable (unique ownership)
 * - Automatically clears state on destruction
 * - Zero overhead when moved (just pointer + size)
 * - Type-safe (can't accidentally use after free)
 *
 * **Phase 2.0 Memory Model**
 * DMAMemoryManager is a slab allocator that doesn't support individual Free().
 * Memory is reclaimed when the entire slab is destroyed during AsyncSubsystem
 * shutdown. PayloadHandle tracks ownership and prevents double-use, but doesn't
 * actually free memory in its destructor.
 *
 * **Usage**
 * \code
 * // Allocate payload
 * auto handle = payloadMgr->Allocate(1024);
 * if (!handle) {
 *     return kIOReturnNoMemory;
 * }
 *
 * // Access payload
 * std::span<uint8_t> data = handle->Data();
 * std::memcpy(data.data(), buffer, size);
 *
 * // Automatic cleanup on scope exit (clears handle state)
 * \endcode
 *
 * **Thread Safety**
 * Not thread-safe. Caller must ensure exclusive access during lifetime.
 */
class PayloadHandle {
public:
    /**
     * \brief Construct empty handle (no payload).
     */
    PayloadHandle() noexcept
        : dmaMgr_(nullptr), address_(0), size_(0), physAddr_(0) {}

    /**
     * \brief Construct handle with allocated payload.
     *
     * \param dmaMgr DMA memory manager (for deallocation)
     * \param address Virtual address of payload
     * \param size Size in bytes
     * \param physAddr Physical address (for DMA)
     */
    PayloadHandle(DMAMemoryManager* dmaMgr, uint64_t address, size_t size, uint64_t physAddr) noexcept
        : dmaMgr_(dmaMgr), address_(address), size_(size), physAddr_(physAddr) {}

    /**
     * \brief Destructor - automatically frees payload.
     */
    ~PayloadHandle() noexcept;

    // Move semantics (transfer ownership)
    PayloadHandle(PayloadHandle&& other) noexcept
        : dmaMgr_(other.dmaMgr_),
          address_(other.address_),
          size_(other.size_),
          physAddr_(other.physAddr_) {
        other.dmaMgr_ = nullptr;
        other.address_ = 0;
        other.size_ = 0;
        other.physAddr_ = 0;
    }

    PayloadHandle& operator=(PayloadHandle&& other) noexcept {
        if (this != &other) {
            Release();  // Free current payload
            dmaMgr_ = other.dmaMgr_;
            address_ = other.address_;
            size_ = other.size_;
            physAddr_ = other.physAddr_;
            other.dmaMgr_ = nullptr;
            other.address_ = 0;
            other.size_ = 0;
            other.physAddr_ = 0;
        }
        return *this;
    }

    // Delete copy semantics (unique ownership)
    PayloadHandle(const PayloadHandle&) = delete;
    PayloadHandle& operator=(const PayloadHandle&) = delete;

    /**
     * \brief Check if handle owns a payload.
     */
    [[nodiscard]] explicit operator bool() const noexcept {
        return address_ != 0 && size_ > 0;
    }

    /**
     * \brief Get mutable payload data.
     */
    [[nodiscard]] std::span<uint8_t> Data() noexcept {
        return std::span<uint8_t>(reinterpret_cast<uint8_t*>(address_), size_);
    }

    /**
     * \brief Get const payload data.
     */
    [[nodiscard]] std::span<const uint8_t> Data() const noexcept {
        return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(address_), size_);
    }

    /**
     * \brief Get virtual address (for descriptor creation).
     */
    [[nodiscard]] uint64_t Address() const noexcept {
        return address_;
    }

    /**
     * \brief Get physical address (for DMA).
     */
    [[nodiscard]] uint64_t PhysicalAddress() const noexcept {
        return physAddr_;
    }

    /**
     * \brief Get size in bytes.
     */
    [[nodiscard]] size_t Size() const noexcept {
        return size_;
    }

    /**
     * \brief Manually release payload (clears handle state).
     *
     * Normally called automatically by destructor. Use this for explicit control.
     * Note: Does not free memory (Phase 2.0 slab allocator model).
     */
    void Release() noexcept;

    // ========================================================================
    // PayloadType Concept Compliance (Phase 1.3)
    // ========================================================================

    /**
     * \brief Get mutable buffer view (PayloadType concept).
     * Alias for Data() to satisfy PayloadType concept requirements.
     */
    [[nodiscard]] std::span<uint8_t> GetBuffer() noexcept {
        return Data();
    }

    /**
     * \brief Get const buffer view (PayloadType concept).
     * Alias for Data() to satisfy PayloadType concept requirements.
     */
    [[nodiscard]] std::span<const uint8_t> GetBuffer() const noexcept {
        return Data();
    }

    /**
     * \brief Get IOVA (device-visible physical address) (PayloadType concept).
     * Alias for PhysicalAddress() to satisfy PayloadType concept requirements.
     */
    [[nodiscard]] uint64_t GetIOVA() const noexcept {
        return PhysicalAddress();
    }

    /**
     * \brief Get size in bytes (PayloadType concept).
     * Alias for Size() to satisfy PayloadType concept requirements.
     */
    [[nodiscard]] size_t GetSize() const noexcept {
        return Size();
    }

    /**
     * \brief Check if handle is valid (PayloadType concept).
     * Alias for operator bool() to satisfy PayloadType concept requirements.
     */
    [[nodiscard]] bool IsValid() const noexcept {
        return address_ != 0 && size_ > 0;
    }

    /**
     * \brief Detach ownership (caller takes responsibility for tracking).
     *
     * \return Virtual address (caller must track manually)
     *
     * **Use Case**
     * When payload outlives the transaction (e.g., queued for deferred processing).
     * Note: Memory is reclaimed when DMAMemoryManager slab is destroyed.
     */
    [[nodiscard]] uint64_t Detach() noexcept {
        uint64_t addr = address_;
        dmaMgr_ = nullptr;
        address_ = 0;
        size_ = 0;
        physAddr_ = 0;
        return addr;
    }

private:
    DMAMemoryManager* dmaMgr_;  // For deallocation (nullptr = detached/empty)
    uint64_t address_;          // Virtual address
    size_t size_;               // Size in bytes
    uint64_t physAddr_;         // Physical address (for DMA)
};

// ============================================================================
// Compile-Time Validation (Phase 1.3)
// ============================================================================

// Validate PayloadHandle satisfies PayloadType concept
static_assert(PayloadType<PayloadHandle>,
              "PayloadHandle must satisfy PayloadType concept");

// Validate UniquePayload is lightweight
static_assert(sizeof(UniquePayload<PayloadHandle>) <= 64,
              "UniquePayload<PayloadHandle> must be lightweight");

// Validate BorrowedPayload is just a reference
static_assert(sizeof(BorrowedPayload<PayloadHandle>) == sizeof(const PayloadHandle*),
              "BorrowedPayload<PayloadHandle> must be just a reference");

} // namespace ASFW::Shared
