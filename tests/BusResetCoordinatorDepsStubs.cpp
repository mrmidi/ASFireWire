#include "ASFWDriver/Bus/SelfIDCapture.hpp"
#include "ASFWDriver/Bus/BusManager.hpp"
#include "ASFWDriver/ConfigROM/ConfigROMStager.hpp"
#include "ASFWDriver/ConfigROM/ROMScanner.hpp"

#include <expected>

namespace ASFW::Driver {

SelfIDCapture::SelfIDCapture() = default;
SelfIDCapture::~SelfIDCapture() = default;

kern_return_t SelfIDCapture::PrepareBuffers(size_t, HardwareInterface&) { return kIOReturnSuccess; }

void SelfIDCapture::ReleaseBuffers() {}

kern_return_t SelfIDCapture::Arm(HardwareInterface&) { return kIOReturnSuccess; }

void SelfIDCapture::Disarm(HardwareInterface&) {}

std::expected<SelfIDCapture::Result, SelfIDCapture::DecodeError>
SelfIDCapture::Decode(uint32_t selfIDCountReg, HardwareInterface&) const {
    return std::unexpected(
        DecodeError{DecodeErrorCode::BufferUnavailable, selfIDCountReg, 0U, 0U});
}

const char* SelfIDCapture::DecodeErrorCodeString(DecodeErrorCode code) noexcept {
    switch (code) {
    case DecodeErrorCode::BufferUnavailable:
        return "BufferUnavailable";
    case DecodeErrorCode::ControllerErrorBit:
        return "ControllerErrorBit";
    case DecodeErrorCode::EmptyCapture:
        return "EmptyCapture";
    case DecodeErrorCode::CountOverflow:
        return "CountOverflow";
    case DecodeErrorCode::NullMapAddress:
        return "NullMapAddress";
    case DecodeErrorCode::GenerationMismatch:
        return "GenerationMismatch";
    case DecodeErrorCode::InvalidInversePair:
        return "InvalidInversePair";
    case DecodeErrorCode::MalformedSequence:
        return "MalformedSequence";
    }
    return "Unknown";
}

ConfigROMStager::ConfigROMStager() = default;
ConfigROMStager::~ConfigROMStager() = default;

kern_return_t ConfigROMStager::Prepare(HardwareInterface&, size_t) { return kIOReturnSuccess; }

kern_return_t ConfigROMStager::StageImage(const ConfigROMBuilder&, HardwareInterface&) {
    return kIOReturnSuccess;
}

void ConfigROMStager::Teardown(HardwareInterface&) {}

void ConfigROMStager::RestoreHeaderAfterBusReset() {}

std::optional<BusManager::PhyConfigCommand> BusManager::AssignCycleMaster(
    const TopologySnapshot&, const std::vector<bool>&) {
    return std::nullopt;
}

std::optional<BusManager::PhyConfigCommand> BusManager::OptimizeGapCount(
    const TopologySnapshot&, const std::vector<uint32_t>&) {
    return std::nullopt;
}

} // namespace ASFW::Driver

namespace ASFW::Discovery {

void ROMScanner::Abort(Generation) {}

} // namespace ASFW::Discovery
