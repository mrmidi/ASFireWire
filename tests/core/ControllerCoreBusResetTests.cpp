#include <gtest/gtest.h>

#include "ASFWDriver/Async/DMAMemoryImpl.hpp"
#include "ASFWDriver/Async/FireWireBusImpl.hpp"
#include "ASFWDriver/Bus/BusManager/BusManagerPolicyCoordinator.hpp"
#include "ASFWDriver/Bus/BusManager/CyclePolicyCoordinator.hpp"
#include "ASFWDriver/Bus/BusManager/GapPolicyCoordinator.hpp"
#include "ASFWDriver/Bus/BusManager/PowerLinkPolicyCoordinator.hpp"
#include "ASFWDriver/Bus/BusManager/RootSelectionCoordinator.hpp"
#include "ASFWDriver/Bus/BusResetCoordinator.hpp"
#include "ASFWDriver/Bus/CSR/BroadcastChannelCSR.hpp"
#include "ASFWDriver/Bus/CSR/SpeedMapService.hpp"
#include "ASFWDriver/Bus/IRM/IRMFallbackCoordinator.hpp"
#include "ASFWDriver/Bus/IRM/LocalIRMResourceController.hpp"
#include "ASFWDriver/ConfigROM/ConfigROMStore.hpp"
#include "ASFWDriver/Controller/ControllerCore.hpp"
#include "ASFWDriver/Controller/ControllerStateMachine.hpp"
#include "ASFWDriver/Diagnostics/DiagnosticLogger.hpp"
#include "ASFWDriver/Discovery/DeviceManager.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Hardware/HardwareInterface.hpp"
#include "ASFWDriver/Hardware/InterruptManager.hpp"
#include "ASFWDriver/Hardware/OHCIEventCodes.hpp"
#include "ASFWDriver/Protocols/AVC/AVCDiscovery.hpp"
#include "ASFWDriver/Protocols/AVC/CMP/CMPClient.hpp"
#include "ASFWDriver/Protocols/SBP2/Session/SessionRegistry.hpp"

#include <utility>

namespace {

ASFW::Discovery::ConfigROM MakeROM(ASFW::Discovery::Generation generation,
                                   uint8_t nodeId,
                                   ASFW::Discovery::Guid64 guid) {
    ASFW::Discovery::ConfigROM rom{};
    rom.gen = generation;
    rom.nodeId = nodeId;
    rom.bib.guid = guid;
    rom.rawQuadlets = {0x31333934u};
    return rom;
}

} // namespace

// This target links the production ControllerCore::HandleInterrupt
// implementation without the DriverKit lifecycle. These narrow host definitions
// keep every unrelated dependency absent while preserving the real bus-reset
// dispatch path under test.
namespace ASFW::Driver {

ControllerCore::ControllerCore(ControllerConfig config, RolePolicy initialPolicy,
                               Dependencies deps)
    : config_(std::move(config)),
      rolePolicy_(initialPolicy),
      deps_(std::move(deps)),
      running_(true) {}

ControllerCore::~ControllerCore() = default;

ControllerStateMachine::ControllerStateMachine() = default;
ControllerStateMachine::~ControllerStateMachine() = default;
ControllerState ControllerStateMachine::CurrentState() const {
    return ControllerState::kRunning;
}
std::string_view ToString(ControllerState) { return "Running"; }

const ControllerStateMachine& ControllerCore::StateMachine() const {
    static ControllerStateMachine stateMachine;
    return stateMachine;
}

void ControllerCore::EvaluateCyclePolicy() noexcept {}
void ControllerCore::HandleCycle64Seconds() {}
void ControllerCore::CompleteRootCycleLostWindow(uint32_t, uint32_t, bool) {}
void ControllerCore::DiagnoseUnrecoverableError() const {}

void ControllerCore::ForceRootAndReset(uint8_t, Role::RoleResetFlavor, uint8_t, uint32_t) {}
void ControllerCore::EnableRemoteCycleMaster(uint8_t, uint32_t) {}
void ControllerCore::EnableLocalCycleMaster(uint32_t) {}
void ControllerCore::ClearLocalContenderAndDelegate(uint8_t, uint32_t) {}
void ControllerCore::OnLocalWonBM(uint32_t, uint8_t) {}
void ControllerCore::OnRemoteBM(uint32_t, uint8_t) {}
void ControllerCore::OnBMElectionFailed(uint32_t, Async::AsyncStatus) {}
void ControllerCore::SendRemoteCmstr(uint8_t, uint32_t) {}
bool ControllerCore::EnableLocalCycleMasterMutation(uint32_t) { return false; }
bool ControllerCore::ClearLocalCycleMasterMutation(uint32_t) { return false; }
Async::AsyncHandle ControllerCore::WriteRemoteStateSetCmstr(uint32_t, uint16_t, uint8_t) {
    return {};
}
bool ControllerCore::ForceRootAndResetForBMPolicy(uint32_t, uint8_t, bool,
                                                  std::optional<uint8_t>) {
    return false;
}
bool ControllerCore::ForceRootAndGapResetForBMPolicy(uint32_t, uint8_t, bool, uint8_t) {
    return false;
}
bool ControllerCore::SendLinkOnPacket(uint32_t, uint16_t, uint8_t) { return false; }

uint32_t InterruptManager::EnabledMask() const { return 0xFFFFFFFFu; }

std::string DiagnosticLogger::DecodeInterruptEvents(uint32_t) { return {}; }

void BusResetCoordinator::OnIrq(uint32_t, uint64_t) {}

namespace Role {

RoleAction EvaluateRolePolicy(const RoleInputs&) noexcept { return {}; }
bool CycleObserver::OnInterrupt(uint32_t, uint32_t) noexcept { return false; }
void RoleCoordinator::OnCycleStartEvidence(uint32_t, CycleObservation) {}

} // namespace Role

} // namespace ASFW::Driver

namespace ASFW::CMP {

void CMPClient::InvalidateAllLeasesForBusReset() {}

} // namespace ASFW::CMP

namespace ASFW::Bus {

void SpeedMapService::Invalidate(uint32_t) noexcept {}
void GapPolicyCoordinator::OnBusResetStarted(uint32_t) noexcept {}
void CyclePolicyCoordinator::OnBusResetStarted(uint32_t) noexcept {}
void IRMFallbackCoordinator::OnBusResetStarted(uint32_t) noexcept {}
void BusManagerElectionDriver::OnBusReset() noexcept {}
void RootSelectionCoordinator::OnBusResetStarted(uint32_t) noexcept {}
void LocalIRMResourceController::OnBusResetStarted(uint32_t) noexcept {}
void PowerLinkPolicyCoordinator::OnBusResetStarted(uint32_t) noexcept {}

} // namespace ASFW::Bus

namespace ASFW::Discovery {

void DeviceManager::SuspendAllForBusReset() {}
void DeviceRegistry::InvalidateLiveMappingsForBusReset() {}

} // namespace ASFW::Discovery

namespace ASFW::Protocols::AVC {

void AVCDiscovery::OnBusReset(uint32_t) {}

} // namespace ASFW::Protocols::AVC

namespace ASFW::Protocols::SBP2 {

void SessionRegistry::OnBusReset(uint16_t) {}

} // namespace ASFW::Protocols::SBP2

TEST(ControllerCoreBusResetTests, InterruptSuspendsCachedROMBeforeRediscovery) {
    using ASFW::Discovery::Generation;

    auto hardware = std::make_shared<ASFW::Driver::HardwareInterface>();
    auto romStore = std::make_shared<ASFW::Discovery::ConfigROMStore>();

    constexpr Generation kCachedGeneration{2};
    constexpr uint32_t kResetGeneration = 3;
    constexpr uint8_t kNodeId = 2;
    constexpr ASFW::Discovery::Guid64 kGuid = 0x00130e0402004713ULL;

    romStore->Insert(MakeROM(kCachedGeneration, kNodeId, kGuid));
    ASSERT_NE(romStore->FindByNode(kCachedGeneration, kNodeId, false), nullptr);

    hardware->SetTestRegister(ASFW::Driver::Register32::kSelfIDGeneration,
                              kResetGeneration);

    ASFW::Driver::ControllerCore::Dependencies deps{};
    deps.hardware = hardware;
    deps.romStore = romStore;
    ASFW::Driver::ControllerCore core(ASFW::Driver::ControllerConfig{},
                                      ASFW::Driver::RolePolicy{}, std::move(deps));

    core.HandleInterrupt(ASFW::Driver::InterruptSnapshot{
        .intEvent = ASFW::Driver::IntEventBits::kBusReset,
        .timestamp = 1,
    });

    // The unfiltered lookup remains available to rediscovery lifecycle code,
    // while selector 14's filtered lookup cannot export stale ROM bytes.
    ASSERT_NE(romStore->FindByNode(kCachedGeneration, kNodeId, true), nullptr);
    EXPECT_EQ(romStore->FindByNode(kCachedGeneration, kNodeId, false), nullptr);
}
