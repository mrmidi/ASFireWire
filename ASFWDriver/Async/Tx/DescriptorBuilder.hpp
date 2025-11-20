#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "../../Shared/Rings/DescriptorRing.hpp"
#include "../../Shared/Memory/DMAMemoryManager.hpp"

namespace ASFW::Async {

// Import Shared types used by DescriptorBuilder
using ASFW::Shared::DescriptorRing;
using ASFW::Shared::DMAMemoryManager;

namespace HW {
struct OHCIDescriptor;
}

// ============================================================================
// CONTRACT: DescriptorBuilder
// ----------------------------------------------------------------------------
// Responsibility:
//   - Convert packet headers + optional payloads into OHCI descriptor chains.
//   - Encode Branch/Control fields per OHCI 1.1 and Agere/LSI quirks:
//       * OUTPUT_MORE relies on physical contiguity (BranchNever, branchWord unused).
//       * OUTPUT_LAST terminates chains with BranchAlways + branchWord==0.
//   - Never overwrite descriptors that hardware may still own.
// Inputs:
//   - DescriptorRing (ring_): exposes At(index) lookups and the [tail, head) free window.
//   - DMAMemoryManager (dmaManager_): publishes cachelines and resolves Virt→IOVA.
// Invariants:
//   - ReserveBlocks(N) returns a contiguous region fully inside the free window or
//     kInvalidRingIndex; it never wraps across live descriptors.
//   - Immediate-only packets consume two descriptor blocks (OHCIDescriptorImmediate)
//     and emit OUTPUT_LAST + BranchAlways + branchWord==0 to mark EOL.
//   - Header+payload packets reserve exactly three blocks: immediate header (OUTPUT_MORE,
//     BranchNever) followed by payload descriptor (OUTPUT_LAST, BranchAlways, branchWord==0).
//   - LinkChain/LinkTailTo patch ONLY the prior OUTPUT_LAST descriptor: branchWord first,
//     release fence, then control.b forced to BranchAlways, followed by PublishRange().
//   - UnlinkTail reverts branchWord→0 while leaving control.b=BranchAlways to restore EOL.
// Threading:
//   - DescriptorBuilder itself is not thread-safe; callers serialize access relative to
//     DescriptorRing head/tail updates. Branch patch helpers are safe while AT RUNNING
//     because they only touch coherent cachelines and publish them immediately.
// Error handling:
//   - BuildTransactionChain returns an Empty() chain on validation/space failures; callers
//     must treat Empty() as "no submission".
// Ownership:
//   - DescriptorBuilder never advances ring head/tail; ContextManager owns that policy.
//   - DMA buffers stay owned by DMAMemoryManager; DescriptorBuilder only flushes ranges.
// ============================================================================

class DescriptorBuilder {
public:
    struct DescriptorChain {
        HW::OHCIDescriptor* first{nullptr};
        HW::OHCIDescriptor* last{nullptr};
        uint32_t firstIOVA32{0};
        uint32_t lastIOVA32{0};
        uint8_t firstBlocks{0};
        uint8_t lastBlocks{0};
        size_t firstRingIndex{0};  ///< Ring index of first descriptor (for release)
        size_t lastRingIndex{0};   ///< Ring index of last descriptor (for release)
        bool needsFlush{false};    ///< Per-descriptor flush flag (Apple offset +40 pattern)
                                    ///< true=block ops with DMA (stop after submit)
                                    ///< false=simple quadlet ops (stop when queue empties)
        uint32_t txid{0};          ///< Monotonic submit identifier (diagnostics)

        [[nodiscard]] uint8_t TotalBlocks() const noexcept {
            uint8_t total = first ? firstBlocks : 0;
            if (last && last != first) {
                total = static_cast<uint8_t>(total + lastBlocks);
            }
            return total;
        }

        [[nodiscard]] bool Empty() const noexcept { return first == nullptr; }
    };

    DescriptorBuilder(DescriptorRing& ring, DMAMemoryManager& dmaManager);
    ~DescriptorBuilder() = default;

    [[nodiscard]] DescriptorChain BuildTransactionChain(const void* headerData,
                                                        std::size_t headerSize,
                                                        uint64_t payloadDeviceAddress,
                                                        std::size_t payloadSize,
                                                        bool needsFlush = false);

    void LinkChain(DescriptorChain& chainToModify,
                   uint64_t nextChainIOVA,
                   uint8_t nextChainBlockCount);

    // Lightweight helper APIs for external Submitter orchestration
    // Patch the branchWord of an existing descriptor and flush the single word
    void PatchBranchWord(HW::OHCIDescriptor* descriptor, uint32_t branchWord) noexcept;

    // Flush a contiguous descriptor range starting at `start` for `blocks` 16-byte units
    void FlushDescriptorRange(HW::OHCIDescriptor* start, uint8_t blocks) noexcept;

    // Flush the entire chain (first..last) to memory
    void FlushChain(const DescriptorChain& chain) noexcept;

    // Helper: patch existing tail descriptor at `tailIndex` to point to newChain
    // This is a convenience wrapper which locates the tail descriptor inside the ring,
    // patches branchWord and flushes as necessary.
    bool LinkTailTo(size_t tailIndex, const DescriptorChain& newChain) noexcept;

    // Helper: revert (unlink) the tail descriptor's branch back to EOL state
    // Used when PATH 2→1 fallback occurs to remove stale linkage before re-arming via CommandPtr
    void UnlinkTail(size_t tailIndex) noexcept;

    // Flush a tail descriptor at ring index
    void FlushTail(size_t tailIndex, uint8_t blocks) noexcept;

    // Tag descriptor with software tag (for completion matching)
    // Writes softwareTag field and syncs the descriptor to DMA
    void TagSoftware(HW::OHCIDescriptor* tail, uint32_t tag) noexcept;

    // Note: ReleaseChain() removed - descriptors are managed by ring lifecycle

private:
    DescriptorRing& ring_;
    DMAMemoryManager& dmaManager_;
    size_t nextAllocationIndex_{0};  ///< Track next free ring slot for allocation
    std::atomic<uint32_t> txCounter_{0};

    [[nodiscard]] size_t ReserveBlocks(uint8_t blocks) noexcept;
};

} // namespace ASFW::Async
