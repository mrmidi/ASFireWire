// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// Link stubs for the ControllerCore interrupt harness. These are subsystems
// ControllerCore reaches only through dependency pointers the harness leaves
// null (discovery, AV/C, CMP, IRM client, SBP-2, audio). Each fails the test
// if it is called, so a stub can never stand in for behaviour under test.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Core/AudioRuntimeRegistry.hpp"
#include "ASFWDriver/Bus/IRM/IRMClient.hpp"
#include "ASFWDriver/ConfigROM/ConfigROMStager.hpp"
#include "ASFWDriver/ConfigROM/ROMScanner.hpp"
#include "ASFWDriver/Discovery/DeviceManager.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Protocols/AVC/AVCDiscovery.hpp"
#include "ASFWDriver/Protocols/AVC/CMP/CMPClient.hpp"
#include "ASFWDriver/Protocols/SBP2/Session/SessionRegistry.hpp"

#define ASFW_UNEXPECTED_CALL() ADD_FAILURE() << "unexpected call: " << __func__

namespace ASFW::Driver {
kern_return_t ConfigROMStager::StageImage(const ConfigROMBuilder&, HardwareInterface&) {
    ASFW_UNEXPECTED_CALL();
    return kIOReturnUnsupported;
}
void ConfigROMStager::RestoreHeaderAfterBusReset() { ASFW_UNEXPECTED_CALL(); }
} // namespace ASFW::Driver

namespace ASFW::Discovery {
bool ROMScanner::Start(const ROMScanRequest&, ScanCompletionCallback) {
    ASFW_UNEXPECTED_CALL();
    return false;
}
void ROMScanner::Abort(Generation) { ASFW_UNEXPECTED_CALL(); }
void ROMScanner::SetTopologyManager(Driver::TopologyManager*) { ASFW_UNEXPECTED_CALL(); }
void DeviceManager::SuspendAllForBusReset() { ASFW_UNEXPECTED_CALL(); }
DeviceRecord DeviceRegistry::UpsertFromROM(const ConfigROM&, const LinkPolicy&) {
    ASFW_UNEXPECTED_CALL();
    return {};
}
void DeviceRegistry::MarkDuplicateGuid(Generation, Guid64, uint8_t) { ASFW_UNEXPECTED_CALL(); }
void DeviceRegistry::RetireDevice(Guid64) { ASFW_UNEXPECTED_CALL(); }
void DeviceRegistry::InvalidateLiveMappingsForBusReset() { ASFW_UNEXPECTED_CALL(); }
} // namespace ASFW::Discovery

namespace ASFW::IRM {
void IRMClient::SetIRMNode(uint8_t, Generation, uint64_t) { ASFW_UNEXPECTED_CALL(); }
} // namespace ASFW::IRM

namespace ASFW::CMP {
void CMPClient::InvalidateAllLeasesForBusReset() { ASFW_UNEXPECTED_CALL(); }
} // namespace ASFW::CMP

namespace ASFW::Protocols::AVC {
void AVCDiscovery::OnBusReset(uint32_t) { ASFW_UNEXPECTED_CALL(); }
} // namespace ASFW::Protocols::AVC

namespace ASFW::Protocols::SBP2 {
void SessionRegistry::OnBusReset(uint16_t) { ASFW_UNEXPECTED_CALL(); }
void SessionRegistry::RefreshTargets(Discovery::Generation) { ASFW_UNEXPECTED_CALL(); }
} // namespace ASFW::Protocols::SBP2

namespace ASFW::Audio {
std::shared_ptr<IDeviceProtocol> AudioRuntimeRegistry::EnsureForDevice(
    const Discovery::DeviceRecord&, Async::IFireWireBusOps*, Async::IFireWireBusInfo*,
    Discovery::DeviceRegistry&, IRM::IRMClient*) noexcept {
    ASFW_UNEXPECTED_CALL();
    return nullptr;
}
} // namespace ASFW::Audio
