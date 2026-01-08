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
    (void)entries;
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
    if (state_ != State::Created) {
        return;
    }

    state_ = State::Ready;
}

void FWUnit::Suspend()
{
    if (state_ != State::Ready) {
        return;
    }

    state_ = State::Suspended;
}

void FWUnit::Resume()
{
    if (state_ != State::Suspended) {
        return;
    }

    state_ = State::Ready;
}

void FWUnit::Terminate()
{
    if (state_ == State::Terminated) {
        return;
    }

    state_ = State::Terminated;
}

} // namespace ASFW::Discovery
