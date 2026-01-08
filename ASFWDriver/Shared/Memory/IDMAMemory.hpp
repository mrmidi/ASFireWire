#pragma once
#include <cstdint>
#include <cstddef>
#include <optional>

namespace ASFW::Shared {

/**
 * @brief DMA memory region with CPU virtual and device IOVA addresses.
 *
 * Represents a contiguous DMA-coherent buffer accessible by both CPU and OHCI.
 */
struct DMARegion {
    uint8_t* virtualBase;   ///< CPU-accessible virtual address
    uint64_t deviceBase;    ///< Device-visible IOVA (32-bit for OHCI)
    size_t size;            ///< Region size (16-byte aligned)
};

/**
 * @brief Pure virtual interface for DMA memory allocation and mapping.
 *
 * Wraps DMAMemoryManager to provide:
 * - Sequential allocation from pre-mapped DMA slab
 * - Virtual ↔ IOVA translation
 * - Cache coherency management (publish/fetch)
 *
 * Design Principles:
 * - Cursor-based allocator (no deallocation — regions live until driver unload)
 * - 16-byte alignment enforcement (OHCI descriptor requirement)
 * - Explicit coherency control (CPU must flush before HW access)
 *
 * Consumers: DescriptorBuilder, PayloadRegistry, future isoch buffers
 */
class IDMAMemory {
public:
    virtual ~IDMAMemory() = default;

    // -------------------------------------------------------------------------
    // Allocation
    // -------------------------------------------------------------------------

    /**
     * @brief Allocate DMA-coherent memory region.
     *
     * @param size Bytes to allocate (will be rounded up to alignment)
     * @param alignment Desired start address alignment (must be power of 2, min 16).
     *                  Larger alignments are supported and will consume extra padding.
     * @return DMARegion on success, std::nullopt if insufficient space
     *
     * Note: Allocation is permanent (no free). Driver allocates 2MB slab at init.
     *
     * Thread Safety: Intended for init-time use only; not currently locked.
     */
    virtual std::optional<DMARegion> AllocateRegion(
        size_t size,
        size_t alignment = 16) = 0;

    // -------------------------------------------------------------------------
    // Address Translation
    // -------------------------------------------------------------------------

    /**
     * @brief Convert CPU virtual address to device IOVA.
     *
     * @param virt Virtual address (must be within allocated slab)
     * @return IOVA address for OHCI descriptor programming
     *
     * Precondition: [virt] must be from a previously allocated DMARegion.
     * Behavior is undefined for addresses outside the DMA slab.
     */
    virtual uint64_t VirtToIOVA(const void* virt) const noexcept = 0;

    /**
     * @brief Convert device IOVA to CPU virtual address.
     *
     * @param iova IOVA address (from OHCI register or descriptor)
     * @return Virtual address for CPU access
     *
     * Precondition: [iova] must be within allocated slab range.
     */
    virtual void* IOVAToVirt(uint64_t iova) const noexcept = 0;

    // -------------------------------------------------------------------------
    // Cache Coherency
    // -------------------------------------------------------------------------

    /**
     * @brief Ensure ordering of CPU writes before device access.
     *
     * @param address Start of memory range (must be 16-byte aligned)
     * @param length Bytes to flush (will be rounded up to cache line)
     *
     * For uncached mappings (current implementation) this is a lightweight
     * memory barrier; no cache flush is needed.
     */
    virtual void PublishToDevice(const void* address, size_t length) const noexcept = 0;

    /**
     * @brief Ensure ordering of device writes before CPU reads.
     *
     * @param address Start of memory range
     * @param length Bytes to invalidate
     *
     * For uncached mappings (current implementation) this is a lightweight
     * memory barrier; no cache invalidation is needed.
     */
    virtual void FetchFromDevice(const void* address, size_t length) const noexcept = 0;

    // -------------------------------------------------------------------------
    // Resource Queries
    // -------------------------------------------------------------------------

    /**
     * @brief Get total DMA slab size.
     *
     * @return Total bytes allocated at driver init (typically 2MB)
     */
    virtual size_t TotalSize() const noexcept = 0;

    /**
     * @brief Get remaining unallocated space.
     *
     * @return Available bytes (decreases with each AllocateRegion call)
     */
    virtual size_t AvailableSize() const noexcept = 0;
};

} // namespace ASFW::Shared
