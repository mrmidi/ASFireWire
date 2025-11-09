#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ASFW::Async {

class DescriptorRing;
class DMAMemoryManager;

namespace HW {
struct OHCIDescriptor;
}

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
    void LinkTailTo(size_t tailIndex, const DescriptorChain& newChain) noexcept;

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
