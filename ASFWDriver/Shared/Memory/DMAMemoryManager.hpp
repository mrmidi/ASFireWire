#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSSharedPtr.h>

namespace ASFW::Driver {
class HardwareInterface;
}

namespace ASFW::Shared {

/**
 * \brief DMA memory slab manager for OHCI descriptor rings and buffers.
 *
 * Allocates a single contiguous DMA region and partitions it into sub-regions
 * for AT/AR descriptor rings and AR data buffers. Provides physical/virtual
 * address translation for descriptor chaining.
 *
 * \par OHCI Specification References
 * - §1.7 Table 7-3: Descriptors must be 16-byte aligned
 * - §7.1: AT descriptors fetched via PCI in host byte order
 * - §8.4.2: AR buffers written by hardware in big-endian format
 *
 * \par Apple Pattern
 * Similar to AppleFWOHCI::setupAsync() which allocates two IOMemory blocks
 * for TX/RX regions (observed at object offsets +673/+799).
 *
 * \par Design Rationale
 * - **Single allocation**: Reduces fragmentation, simplifies lifecycle
 * - **Sequential partitioning**: Cursor-based allocator for deterministic layout
 * - **RAII ownership**: IODMACommand must stay alive to maintain IOMMU mapping
 *
 * \note This class is not thread-safe. AllocateRegion() must be called
 *       sequentially during AsyncSubsystem initialization.
 */
class DMAMemoryManager {
public:
    /**
     * \brief Allocated DMA region descriptor.
     */
    struct Region {
        uint8_t* virtualBase;   ///< CPU-accessible virtual address
        uint64_t deviceBase;    ///< Device-visible IOVA (guaranteed 32-bit safe)
        size_t size;            ///< Region size in bytes (16-byte aligned)
    };

    DMAMemoryManager() = default;
    ~DMAMemoryManager();

    // Deterministic unmap/release of DMA resources. Safe to call multiple times.
    void Reset() noexcept;

    /**
     * \brief Initialize DMA slab with specified total size.
     *
     * Allocates a contiguous DMA-capable memory region via HardwareInterface,
     * ensures 16-byte alignment, and zeroes the entire slab.
     *
     * \param hw Hardware interface for DMA allocation
     * \param totalSize Total slab size in bytes (will be rounded up to 16-byte alignment)
     * \return true on success, false on allocation failure
     *
     * \par Implementation Details
     * - Calls HardwareInterface::AllocateDMA() with kIOMemoryDirectionInOut
     * - Validates physical address fits in 32-bit space (OHCI limitation)
     * - Maintains IODMACommand alive to preserve IOMMU mapping
     * - Zeroes entire slab for deterministic descriptor state
     *
     * \par OHCI Requirement
     * Per §1.7: "All descriptor blocks must be 16-byte aligned and reside
     * within the first 4GB of physical address space."
     */
    [[nodiscard]] bool Initialize(Driver::HardwareInterface& hw, size_t totalSize);

    /**
     * \brief Allocate a sub-region from the slab.
     *
     * Partitions the slab using a sequential cursor-based allocator.
     * Automatically enforces 16-byte alignment for all regions.
     *
     * \param size Desired region size in bytes (will be rounded up to 16-byte alignment)
     * \return Region descriptor on success, std::nullopt if insufficient space
     *
     * \par Usage Pattern
     * Called sequentially during initialization to partition slab:
     * 1. AT Request descriptors (256 × 32 bytes = 8KB)
     * 2. AT Response descriptors (64 × 32 bytes = 2KB)
     * 3. AR Request descriptors + buffers (128 × (16 + 4096) bytes ≈ 512KB)
     * 4. AR Response descriptors + buffers (256 × (16 + 4096) bytes ≈ 1MB)
     *
     * \warning Once a region is allocated, it cannot be freed individually.
     *          The entire slab is released on destruction.
     */
    [[nodiscard]] std::optional<Region> AllocateRegion(size_t size);

    /**
     * \brief Convert virtual address to physical address.
     *
     * Translates a CPU-accessible virtual pointer to a bus-visible physical
     * address suitable for OHCI descriptor fields (branchWord, dataAddress).
     *
     * \param virt Virtual address (must be within slab range)
     * \return Physical address, or 0 if virt is out-of-bounds
     *
     * \par OHCI Usage
     * Used to populate descriptor branchWord fields per §7.1.5.1 Table 7-3:
     * \code
     * descriptor->branchWord = MakeBranchWordAT(
     *     dmaManager->VirtToIOVA(nextDescriptor),
     *     blocksCount
     * );
     * \endcode
     *
     * \par Performance
     * O(1) pointer arithmetic (no table lookup required).
     */
    [[nodiscard]] uint64_t VirtToIOVA(const void* virt) const noexcept;

    /**
     * \brief Convert physical address to virtual address.
     *
     * Translates a bus-visible physical address back to a CPU-accessible
     * pointer. Used during descriptor completion scanning.
     *
     * \param iova Device-visible address (must be within slab range)
     * \return Virtual address, or nullptr if the address is out-of-bounds
     *
     * \par OHCI Usage
     * Used to decode descriptor branchWord during completion scanning:
     * \code
     * uint32_t nextPhys = HW::DecodeBranchPhys32_AT(descriptor->branchWord);
     * auto* nextDesc = dmaManager->IOVAToVirt(nextPhys);
     * \endcode
     *
     * \par Apple Pattern
     * Similar to ChannelBundle::DescriptorFromPhys32() in original implementation.
     *
     * \par Performance
     * O(1) pointer arithmetic (no table lookup required).
     */
    [[nodiscard]] void* IOVAToVirt(uint64_t iova) const noexcept;

    /**
     * \brief Enable or disable verbose DMA coherency tracing.
     *
     * When enabled, PublishRange/FetchRange emit detailed diagnostics including
     * offsets, aligned lengths, and hex dumps of the data that is being pushed
     * to or pulled from device-visible memory.
     */
    static void SetTracingEnabled(bool enabled) noexcept;

    /// Query whether coherency tracing is currently active.
    [[nodiscard]] static bool IsTracingEnabled() noexcept;

    /**
     * \brief Publish CPU descriptor writes to DMA-visible memory.
     *
     * Ensures write ordering before setting hardware RUN/WAKE bits.
     * With uncached mapping (kIOMemoryMapCacheModeInhibit), this is just
     * a memory barrier - no actual flush needed.
     *
     * \param address Start of range to publish (must be within slab)
     * \param length Length in bytes
     */
    void PublishRange(const void* address, size_t length) const noexcept;

    /**
     * \brief Fetch device-written data into CPU view.
     *
     * Ensures read ordering after hardware completion.
     * With uncached mapping (kIOMemoryMapCacheModeInhibit), this is just
     * a memory barrier - no actual invalidation needed.
     *
     * \param address Start of range to fetch (must be within slab)
     * \param length Length in bytes
     */
    void FetchRange(const void* address, size_t length) const noexcept;

    /// Diagnostic: Dump 64-byte cache-line-aligned region for visibility testing
    void HexDump64(const void* address, const char* tag) const noexcept;

    /// Total slab size in bytes (16-byte aligned)
    [[nodiscard]] size_t TotalSize() const noexcept { return slabSize_; }

    /// Remaining unallocated bytes in slab
    [[nodiscard]] size_t AvailableSize() const noexcept { return slabSize_ - cursor_; }

    /// Base virtual address of DMA slab
    [[nodiscard]] uint8_t* BaseVirtual() const noexcept { return slabVirt_; }

    /// Base IOVA of DMA slab (device-visible address)
    [[nodiscard]] uint64_t BaseIOVA() const noexcept { return slabIOVA_; }

    DMAMemoryManager(const DMAMemoryManager&) = delete;
    DMAMemoryManager& operator=(const DMAMemoryManager&) = delete;

private:
    /// Round size up to 16-byte alignment (OHCI requirement)
    [[nodiscard]] static constexpr size_t AlignSize(size_t size) noexcept {
        return (size + 15) & ~size_t(15);
    }

    /// Check if pointer is within slab bounds
    [[nodiscard]] bool IsInSlabRange(const void* ptr) const noexcept;

    /// Check if physical address is within slab bounds
    [[nodiscard]] bool IsInSlabRange(uint64_t iova) const noexcept;

    /// Zero the slab using volatile stores (uncached mapping rejects dc zva)
    void ZeroSlab(size_t length) noexcept;

    void TraceHexPreview(const char* tag,
                         const void* address,
                         size_t length) const noexcept;

    /// DMA buffer (DriverKit-managed memory)
    OSSharedPtr<IOBufferMemoryDescriptor> dmaBuffer_;

    /// DMA command (CRITICAL: must stay alive to maintain IOMMU mapping)
    OSSharedPtr<IODMACommand> dmaCommand_;

    /// Virtual memory mapping (CPU-accessible)
    IOMemoryMap* dmaMemoryMap_{nullptr};

    uint8_t* slabVirt_{nullptr};    ///< Virtual base address
    uint64_t slabIOVA_{0};          ///< Device-visible base address (IOVA)
    size_t slabSize_{0};            ///< Total slab size (aligned)
    size_t mappingLength_{0};       ///< Length of prepared DMA mapping
    size_t cursor_{0};              ///< Current allocation offset
};

} // namespace ASFW::Shared
