#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../OHCI_HW_Specs.hpp"
#include "RingHelpers.hpp"  // Phase 2.4: Shared ring utilities

namespace ASFW::Async {

/**
 * \brief Lock-free circular ring buffer for OHCI DMA descriptors.
 *
 * Manages a fixed-size ring of OHCI descriptors with atomic head/tail pointers
 * for concurrent reads (hardware/software scanning) while serializing writes
 * through external locking.
 *
 * \par OHCI Specification References
 * - §7.1: AT (Asynchronous Transmit) descriptor formats
 * - §7.1.5.1: branchWord field for descriptor linking
 * - Table 7-5: Descriptor block Z values (2-15 for OUTPUT_*, 0=end-of-list)
 *
 * \par Design Rationale
 * - **Lock-free reads**: Hardware and software can scan completed descriptors
 *   without contention (atomic head/tail allow concurrent readers)
 * - **External write lock**: SubmitChain() callers must serialize via ATContextBase lock
 * - **No push/pop**: AT contexts manually link descriptors via branchWord, so
 *   ring only tracks head/tail indices, not ownership transfer
 *
 * \par Apple Pattern
 * Similar to ChannelBundle's descriptor ring tracking in AppleFWOHCI:
 * - Circular buffer with head/tail cursors
 * - No sentinel descriptor (contexts arm on-demand, see DECOMPILATION.md)
 * - Hardware advances tail via branchWord, software scans from head
 *
 * \par Linux Pattern
 * See drivers/firewire/ohci.c:
 * - context_append(): Appends descriptors by updating tail->branchWord
 * - context_tasklet(): Scans completed descriptors from head to tail
 *
 * \warning Capacity is fixed at initialization. Once full, new descriptors
 *          cannot be submitted until completed ones are freed.
 */
class DescriptorRing {
public:
    DescriptorRing() = default;
    ~DescriptorRing() = default;

    /**
     * \brief Initialize ring with descriptor storage.
     *
     * Zeroes all descriptors and prepares ring for use. Per Apple's implementation,
     * no sentinel descriptor is used. AT contexts arm on-demand during first SubmitChain() call.
     *
     * \param descriptors Storage view (must be 16-byte aligned, at least 1 descriptor)
     * \return true on success, false if descriptors is empty or misaligned
     *
     * \par Implementation
     * - Zeroes all descriptors via memset
     * - Sets head=0, tail=0 (ring initially empty)
     * - Capacity = descriptors.size() (full ring available)
     * - No sentinel programming (matches Apple's approach per DECOMPILATION.md)
     */
    [[nodiscard]] bool Initialize(std::span<HW::OHCIDescriptor> descriptors) noexcept;

    /**
     * \brief Check if ring is empty.
     *
     * \return true if no descriptors are currently in-flight
     *
     * \par Thread Safety
     * Lock-free read via atomic head/tail. Safe to call concurrently with
     * ScanCompletion() but NOT with SubmitChain() (which requires external lock).
     *
     * \par Phase 2.4
     * Uses shared RingHelpers for atomic operations.
     */
    [[nodiscard]] bool IsEmpty() const noexcept {
        return RingHelpers::IsEmptyAtomic(head_, tail_);
    }

    /**
     * \brief Check if ring is full.
     *
     * \return true if no space available for new descriptors
     *
     * \par Implementation
     * Ring is full when (tail+1) % capacity == head. We reserve one slot to
     * distinguish full from empty (both would have head==tail otherwise).
     */
    [[nodiscard]] bool IsFull() const noexcept;

    /**
     * \brief Get maximum number of descriptors ring can hold.
     *
     * \return Usable capacity (storage.size(), full ring available)
     *
     * \par Note
     * Per Apple's implementation, AT contexts arm on-demand without sentinel loops.
     */
    [[nodiscard]] size_t Capacity() const noexcept {
        return capacity_;
    }

    /**
     * \brief Get current number of in-flight descriptors.
     *
     * \return Count of descriptors between head and tail (may be stale)
     */
    [[nodiscard]] size_t Size() const noexcept;

    /**
     * \brief Get descriptor at specified index.
     *
     * \param index Ring index (0 to capacity-1)
     * \return Pointer to descriptor, or nullptr if index out-of-bounds
     *
     * \par Usage
     * AT contexts use this to access descriptors when building chains:
     * \code
     * auto* desc = ring.At(tailIndex);
     * desc->branchWord = MakeBranchWordAT(nextPhys, nextBlocks);
     * \endcode
     */
    [[nodiscard]] HW::OHCIDescriptor* At(size_t index) noexcept;
    [[nodiscard]] const HW::OHCIDescriptor* At(size_t index) const noexcept;

    /**
     * \brief Get current head index (oldest in-flight descriptor).
     *
     * \return Head index (atomic read)
     *
     * \par Thread Safety
     * Lock-free atomic read. Safe to call concurrently with hardware updates.
     */
    [[nodiscard]] size_t Head() const noexcept {
        return head_.load(std::memory_order_acquire);
    }

    /**
     * \brief Get current tail index (next descriptor to submit).
     *
     * \return Tail index (atomic read)
     */
    [[nodiscard]] size_t Tail() const noexcept {
        return tail_.load(std::memory_order_acquire);
    }

    /**
     * \brief Advance head index after processing completed descriptors.
     *
     * \param newHead New head value (must be between current head and tail)
     *
     * \par Usage
     * Called by ScanCompletion() after extracting completion results from
     * descriptors [head, newHead).
     *
     * \warning Caller must ensure newHead is valid (no bounds checking).
     */
    void SetHead(size_t newHead) noexcept {
        head_.store(newHead, std::memory_order_release);
    }

    /**
     * \brief Advance tail index after submitting descriptors.
     *
     * \param newTail New tail value (must advance forward, wrapping allowed)
     *
     * \par Usage
     * Called by SubmitChain() after linking new descriptors into the ring.
     *
     * \warning Caller must ensure newTail is valid and external lock is held.
     */
    void SetTail(size_t newTail) noexcept {
        tail_.store(newTail, std::memory_order_release);
    }

    /**
     * \brief Set the block count of the previous descriptor's last block.
     *
     * Used to correctly link new descriptors when appending to a running context.
     * For immediate descriptors (2 blocks), we need to step back by 2, not 1.
     *
     * \param blocks Block count of the previous descriptor's last block (1 or 2)
     */
    void SetPrevLastBlocks(uint8_t blocks) noexcept {
        prev_last_blocks_.store(blocks, std::memory_order_release);
    }

    /**
     * \brief Get the block count of the previous descriptor's last block.
     *
     * \return Block count (0 if nothing submitted yet, 1 for standard, 2 for immediate)
     */
    [[nodiscard]] uint8_t PrevLastBlocks() const noexcept {
        return prev_last_blocks_.load(std::memory_order_acquire);
    }

    /**
     * \brief Locate the previous chain's LAST descriptor given the current tail.
     *
     * Handles immediate (32-byte) descriptors by rewinding to the header block.
     *
     * \param tailIndex Tail index at which the next submission will occur
     * \param[out] outDescriptor Pointer to previous LAST descriptor header
     * \param[out] outIndex Ring index of the descriptor header
     * \param[out] outBlocks Block count stored for the previous LAST descriptor
     * \return true if a descriptor was located, false if no previous submission exists
     */
    bool LocatePreviousLast(size_t tailIndex,
                            HW::OHCIDescriptor*& outDescriptor,
                            size_t& outIndex,
                            uint8_t& outBlocks) noexcept;

    bool LocatePreviousLast(size_t tailIndex,
                            const HW::OHCIDescriptor*& outDescriptor,
                            size_t& outIndex,
                            uint8_t& outBlocks) const noexcept;

    /**
     * \brief Get raw descriptor storage span.
     *
     * \return View of all descriptors
     */
    [[nodiscard]] std::span<HW::OHCIDescriptor> Storage() noexcept {
        return storage_;
    }

    [[nodiscard]] std::span<const HW::OHCIDescriptor> Storage() const noexcept {
        return storage_;
    }

    // Finalize the ring with the device-visible base address (IOVA) of the descriptor slab.
    // Must be called once the DMAMemoryManager allocation is known.
    [[nodiscard]] bool Finalize(uint64_t descriptorsIOVABase) noexcept;

    // Compute OHCI CommandPtr word for a target descriptor inside this ring.
    // zBlocks is the number of 16-byte blocks at the target (1 or 2 typically).
    [[nodiscard]] uint32_t CommandPtrWordTo(const HW::OHCIDescriptor* target, uint8_t zBlocks) const noexcept;

    // Compute OHCI CommandPtr word given a 32-bit device-visible descriptor address.
    // Returns 0 on error (invalid/unaligned address or ring not finalized).
    [[nodiscard]] uint32_t CommandPtrWordFromIOVA(uint32_t iova32, uint8_t zBlocks) const noexcept;

    DescriptorRing(const DescriptorRing&) = delete;
    DescriptorRing& operator=(const DescriptorRing&) = delete;

private:
    std::span<HW::OHCIDescriptor> storage_;  ///< Descriptor storage (externally owned)
    std::atomic<size_t> head_{0};            ///< Index of oldest in-flight descriptor
    std::atomic<size_t> tail_{0};            ///< Index of next descriptor to submit
    std::atomic<uint8_t> prev_last_blocks_{0}; ///< Block count of previous descriptor's last block (for linking)
    size_t capacity_{0};                     ///< Usable capacity (storage.size())
    uint64_t descIOVABase_{0};               ///< Device-visible base of descriptor storage (set by Finalize)
};

} // namespace ASFW::Async
