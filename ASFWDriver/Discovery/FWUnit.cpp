#include "FWUnit.hpp"
#include "FWDevice.hpp"
#include <algorithm>
#include <cstring>

namespace ASFW::Discovery {

// Private constructor
FWUnit::FWUnit(std::shared_ptr<FWDevice> parentDevice, uint32_t directoryOffset)
    : parentDevice_(std::move(parentDevice))
    , directoryOffset_(directoryOffset)
{
}

// Factory method
std::shared_ptr<FWUnit> FWUnit::Create(
    std::shared_ptr<FWDevice> parentDevice,
    uint32_t directoryOffset,
    const std::vector<RomEntry>& entries)
{
    if (!parentDevice) {
        return nullptr;
    }

    // Use new + shared_ptr constructor (can't use make_shared with private ctor)
    auto unit = std::shared_ptr<FWUnit>(
        new FWUnit(std::move(parentDevice), directoryOffset)
    );

    // Parse ROM entries to extract unit keys
    unit->ParseEntries(entries);

    // Validate required keys are present
    if (unit->unitSpecId_ == 0 || unit->unitSwVersion_ == 0) {
        // Unit directories MUST have Unit_Spec_ID and Unit_SW_Version
        return nullptr;
    }

    // Extract text descriptors (optional)
    unit->ExtractTextLeaves(entries);

    return unit;
}

void FWUnit::ParseEntries(const std::vector<RomEntry>& entries)
{
    for (const auto& entry : entries) {
        switch (entry.key) {
            case CfgKey::Unit_Spec_Id:
                unitSpecId_ = entry.value;
                break;

            case CfgKey::Unit_Sw_Version:
                unitSwVersion_ = entry.value;
                break;

            case CfgKey::Logical_Unit_Number:
                logicalUnitNumber_ = entry.value;
                break;

            case CfgKey::ModelId:
                modelId_ = entry.value;
                break;

            // Other keys (CSR offsets, dependent directories) ignored for now
            default:
                break;
        }
    }
}

void FWUnit::ExtractTextLeaves(const std::vector<RomEntry>& entries)
{
    // IEEE 1212 text descriptor keys (currently unused - for future implementation):
    // constexpr uint8_t kKeyTextLeaf = 0x81;         // Text descriptor leaf
    // constexpr uint8_t kKeyVendorName = 0x03;       // Vendor name text
    // constexpr uint8_t kKeyModelName = 0x17;        // Model name text

    // Text leaves are encoded as:
    // - Key type (immediate or offset)
    // - If offset, points to text leaf structure
    // - Text leaf contains: [spec_id, language, text_data...]
    //
    // For now, we do basic extraction. Full implementation would:
    // 1. Follow offset pointers to text leaf entries
    // 2. Parse text leaf format (language codes, character encoding)
    // 3. Convert to UTF-8 strings
    //
    // Simplified: Look for immediate text entries

    (void)entries;  // Suppress unused parameter warning

    // Text extraction is complex and depends on ROM layout
    // For Phase 1, we defer full text parsing
    // Real implementation would read text leaves from ROM via parent device

    // TODO: Implement full text leaf parsing
    // - Check if entry.key indicates text descriptor
    // - Follow offset to read text leaf from ROM
    // - Parse language code and text encoding
    // - Extract vendor/product strings

    // Placeholder: Use empty strings for now
    // Phase 2 will implement full text parsing when we have ROM reading infrastructure
}

bool FWUnit::Matches(uint32_t specId, std::optional<uint32_t> swVersion) const
{
    // Must match Unit_Spec_ID
    if (unitSpecId_ != specId) {
        return false;
    }

    // If SW version specified, must match exactly
    if (swVersion.has_value() && unitSwVersion_ != *swVersion) {
        return false;
    }

    return true;
}

std::shared_ptr<FWDevice> FWUnit::GetDevice() const
{
    // Parent device is stored as strong reference
    return parentDevice_;
}

// === Lifecycle Methods ===

void FWUnit::Publish()
{
    // Only transition from Created state
    if (state_ != State::Created) {
        return;
    }

    state_ = State::Ready;

    // TODO: Notify observers that unit is published
    // This will be implemented when we add IUnitObserver in Phase 3
}

void FWUnit::Suspend()
{
    // Only transition from Ready state
    if (state_ != State::Ready) {
        return;
    }

    state_ = State::Suspended;

    // TODO: Notify observers that unit is suspended
    // Clients should stop using this unit until Resume() called
}

void FWUnit::Resume()
{
    // Only transition from Suspended state
    if (state_ != State::Suspended) {
        return;
    }

    state_ = State::Ready;

    // TODO: Notify observers that unit is resumed
    // Clients can resume using this unit
}

void FWUnit::Terminate()
{
    // Can transition from any state to Terminated
    if (state_ == State::Terminated) {
        return; // Already terminated
    }

    state_ = State::Terminated;

    // TODO: Notify observers that unit is terminated
    // Clients should release any references and stop using this unit

    // Clear parent reference to break circular dependencies
    // (though in our design, unit holds strong ref so no circular dependency)
    // Keep parent reference intact so GetDevice() still works for cleanup
}

} // namespace ASFW::Discovery
