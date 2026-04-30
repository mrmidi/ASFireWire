#pragma once

// SBP-2 Page Table builder.
// Converts scatter-gather DMA segments into SBP-2 Page Table Entries (PTEs)
// or a single direct-address descriptor when possible.
//
// Ref: SBP-2 §5.1.2 (Page Table Entry format)

#include "AddressSpaceManager.hpp"
#include "SBP2WireFormats.hpp"
#include "../../Logging/Logging.hpp"

#include <cstring>
#include <span>
#include <vector>

namespace ASFW::Protocols::SBP2 {

class SBP2PageTable {
public:
    struct Segment {
        uint64_t address;   // Physical / DMA address of data buffer
        uint32_t length;    // Length in bytes
    };

    struct Result {
        uint32_t dataDescriptorHi{0};
        uint32_t dataDescriptorLo{0};
        uint16_t options{0};    // Page table format bits to OR into ORB options
        uint16_t dataSize{0};   // PTE count for page table, or byte count for direct
        bool isDirect{false};
    };

    explicit SBP2PageTable(AddressSpaceManager& addrMgr, void* owner) noexcept
        : addrMgr_(addrMgr), owner_(owner) {}

    SBP2PageTable(const SBP2PageTable&) = delete;
    SBP2PageTable& operator=(const SBP2PageTable&) = delete;

    /// Build page table from scatter-gather segments.
    /// @param segments       DMA segments describing the data buffer
    /// @param localNodeID    Local node ID for address fields
    /// @param maxPageClipSize Max bytes per PTE entry (default 0xF000 = 60 KiB)
    /// @return true on success
    [[nodiscard]] bool Build(std::span<const Segment> segments,
                             uint16_t localNodeID,
                             uint32_t maxPageClipSize = 0xF000) noexcept {
        Clear();
        const uint16_t busNodeID = Wire::NormalizeBusNodeID(localNodeID);

        if (segments.empty()) {
            result_ = {};
            return true;
        }

        // Clamp max page clip size.
        if (maxPageClipSize == 0 || maxPageClipSize > 0xF000) {
            maxPageClipSize = 0xF000;
        }

        // Build PTE entries into local buffer.
        std::vector<Wire::PageTableEntry> ptes;

        for (const auto& seg : segments) {
            if (seg.length == 0) {
                continue;
            }

            uint64_t phys = seg.address;
            uint32_t remaining = seg.length;

            while (remaining > 0) {
                uint32_t chunk = (remaining > maxPageClipSize) ? maxPageClipSize : remaining;

                Wire::PageTableEntry pte{};
                pte.segmentLength = Wire::ToBE16(static_cast<uint16_t>(chunk));
                pte.segmentBaseAddressHi = Wire::ToBE16(
                    static_cast<uint16_t>((phys >> 32) & 0xFFFFULL));
                pte.segmentBaseAddressLo = Wire::ToBE32(
                    static_cast<uint32_t>(phys & 0xFFFFFFFFULL));

                ptes.push_back(pte);

                phys += chunk;
                remaining -= chunk;
            }
        }

        if (ptes.empty()) {
            result_ = {};
            return true;
        }

        pteCount_ = static_cast<uint32_t>(ptes.size());

        // Optimization: single PTE with quadlet-aligned address → direct mode.
        if (pteCount_ == 1 && (ptes[0].segmentBaseAddressLo & Wire::ToBE32(0x3u)) == 0) {
            result_.dataDescriptorHi = Wire::ToBE32(
                static_cast<uint32_t>(segments.front().address >> 32) & 0xFFFFu);
            result_.dataDescriptorLo = ptes[0].segmentBaseAddressLo;
            result_.dataSize = ptes[0].segmentLength;  // still BE, ORB reads it BE
            result_.options = 0;  // no page table
            result_.isDirect = true;
            return true;
        }

        // Multi-PTE: allocate address space for the page table.
        const uint32_t ptSize = pteCount_ * sizeof(Wire::PageTableEntry);

        auto kr = addrMgr_.AllocateAddressRangeAuto(
            owner_, 0xFFFF, ptSize,
            &pageTableHandle_, &pageTableMeta_);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Async, "SBP2PageTable: failed to allocate page table: 0x%08x", kr);
            return false;
        }

        // Write PTEs into the address space.
        auto pteSpan = std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(ptes.data()), ptSize);
        kr = addrMgr_.WriteLocalData(owner_, pageTableHandle_, 0, pteSpan);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Async, "SBP2PageTable: failed to write page table: 0x%08x", kr);
            Clear();
            return false;
        }

        result_.dataDescriptorHi = Wire::ToBE32(
            Wire::ComposeBusAddressHi(busNodeID, pageTableMeta_.addressHi));
        result_.dataDescriptorLo = Wire::ToBE32(pageTableMeta_.addressLo);
        result_.dataSize = Wire::ToBE16(static_cast<uint16_t>(pteCount_));
        result_.options = Wire::Options::kPageTableUnrestricted;
        result_.isDirect = false;

        ASFW_LOG(Async, "SBP2PageTable: built %u PTEs (%u bytes) at %04x:%08x",
                 pteCount_, ptSize, pageTableMeta_.addressHi, pageTableMeta_.addressLo);
        return true;
    }

    void Clear() noexcept {
        if (pageTableHandle_ != 0) {
            addrMgr_.DeallocateAddressRange(owner_, pageTableHandle_);
            pageTableHandle_ = 0;
        }
        pageTableMeta_ = {};
        result_ = {};
        pteCount_ = 0;
    }

    [[nodiscard]] const Result& GetResult() const noexcept { return result_; }
    [[nodiscard]] uint32_t EntryCount() const noexcept { return pteCount_; }

private:
    AddressSpaceManager& addrMgr_;
    void* owner_;

    uint64_t pageTableHandle_{0};
    AddressSpaceManager::AddressRangeMeta pageTableMeta_{};
    Result result_{};
    uint32_t pteCount_{0};
};

} // namespace ASFW::Protocols::SBP2
