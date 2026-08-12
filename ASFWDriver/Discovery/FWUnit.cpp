#include "FWUnit.hpp"
#include "FWDevice.hpp"
#include <algorithm>
#include <cstring>

namespace ASFW::Discovery {

FWUnit::FWUnit(std::shared_ptr<FWDevice> parentDevice, const UnitDirectory& directory)
    : parentDevice_(parentDevice)
    , parentDeviceId_(parentDevice ? parentDevice->GetInstanceId() : DeviceInstanceId{})
    , directoryOffset_(directory.offsetQuadlets)
    , unitSpecId_(directory.unitSpecId)
    , unitSwVersion_(directory.unitSwVersion)
    , vendorId_(directory.vendorId)
    , modelId_(directory.modelId)
    , logicalUnitNumber_(directory.logicalUnitNumber)
    , managementAgentOffset_(directory.managementAgentOffset)
    , unitCharacteristics_(directory.unitCharacteristics)
    , fastStart_(directory.fastStart)
    , vendorName_(directory.vendorName.value_or(std::string{}))
    , productName_(directory.modelName.value_or(std::string{}))
{
}

// Factory method
std::shared_ptr<FWUnit> FWUnit::Create(
    std::shared_ptr<FWDevice> parentDevice,
    const UnitDirectory& directory)
{
    if (!parentDevice) {
        return nullptr;
    }

    // Use new + shared_ptr constructor (can't use make_shared with private ctor)
    auto unit = std::shared_ptr<FWUnit>(
        new FWUnit(std::move(parentDevice), directory)
    );

    // Validate required keys are present
    if (unit->unitSpecId_ == 0 || unit->unitSwVersion_ == 0) {
        // Unit directories MUST have Unit_Spec_ID and Unit_SW_Version
        return nullptr;
    }

    return unit;
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
