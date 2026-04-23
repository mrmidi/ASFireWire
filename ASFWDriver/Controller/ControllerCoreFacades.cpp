#include "ControllerCore.hpp"

#include <DriverKit/IOLib.h>
#include <cstdio>
#include <string>
#include <unordered_set>

#include "../Async/DMAMemoryImpl.hpp"
#include "../Async/FireWireBusImpl.hpp"
#include "../Async/Interfaces/IAsyncControllerPort.hpp"
#include "../Bus/BusResetCoordinator.hpp"
#include "../Bus/SelfIDCapture.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../ConfigROM/ConfigROMBuilder.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "../ConfigROM/ConfigROMStore.hpp"
#include "../ConfigROM/ROMScanner.hpp"
#include "../Diagnostics/DiagnosticLogger.hpp"
#include "../Diagnostics/MetricsSink.hpp"
#include "../Discovery/DeviceManager.hpp"
#include "../Discovery/DeviceRegistry.hpp"
#include "../Discovery/SpeedPolicy.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Hardware/IEEE1394.hpp"
#include "../Hardware/InterruptManager.hpp"
#include "../Hardware/OHCIConstants.hpp"
#include "../Hardware/OHCIEventCodes.hpp"
#include "../Hardware/RegisterMap.hpp"
#include "../IRM/IRMClient.hpp"
#include "../Protocols/AVC/AVCDiscovery.hpp"
#include "../Protocols/AVC/CMP/CMPClient.hpp"
#include "../Protocols/Audio/DeviceProtocolFactory.hpp"
#include "../Scheduling/Scheduler.hpp"
#include "../Version/DriverVersion.hpp"
#include "ControllerStateMachine.hpp"
#include "Logging.hpp"

namespace ASFW::Driver {

const ControllerStateMachine& ControllerCore::StateMachine() const {
    static ControllerStateMachine placeholder;
    return deps_.stateMachine ? *deps_.stateMachine : placeholder;
}

MetricsSink& ControllerCore::Metrics() const {
    static MetricsSink placeholder{};
    return deps_.metrics ? *deps_.metrics : placeholder;
}

std::optional<TopologySnapshot> ControllerCore::LatestTopology() const {
    if (deps_.topology) {
        auto snapshot = deps_.topology->LatestSnapshot();
        if (snapshot.has_value()) {
            // mute log spamming
            // ASFW_LOG(Controller, "LatestTopology() returning snapshot: gen=%u nodes=%u
            // root=%{public}s IRM=%{public}s",
            //          snapshot->generation,
            //          snapshot->nodeCount,
            //          snapshot->rootNodeId.has_value() ?
            //          std::to_string(*snapshot->rootNodeId).c_str() : "none",
            //          snapshot->irmNodeId.has_value() ?
            //          std::to_string(*snapshot->irmNodeId).c_str() : "none");
        } else {
            ASFW_LOG(Controller, "LatestTopology() returning nullopt (no topology built yet)");
        }
        return snapshot;
    }
    ASFW_LOG(Controller, "LatestTopology() returning nullopt (no TopologyManager)");
    return std::nullopt;
}

Discovery::ConfigROMStore* ControllerCore::GetConfigROMStore() const {
    return deps_.romStore.get();
}

Discovery::ROMScanner* ControllerCore::GetROMScanner() const { return deps_.romScanner.get(); }

void ControllerCore::AttachROMScanner(std::shared_ptr<Discovery::ROMScanner> romScanner) {
    deps_.romScanner = std::move(romScanner);
}

Discovery::IDeviceManager* ControllerCore::GetDeviceManager() const {
    return deps_.deviceManager.get();
}

Discovery::IUnitRegistry* ControllerCore::GetUnitRegistry() const {
    return deps_.deviceManager.get();
}

Discovery::DeviceRegistry* ControllerCore::GetDeviceRegistry() const {
    return deps_.deviceRegistry.get();
}

Protocols::AVC::IAVCDiscovery* ControllerCore::GetAVCDiscovery() const {
    return deps_.avcDiscovery.get();
}

void ControllerCore::SetAVCDiscovery(std::shared_ptr<Protocols::AVC::AVCDiscovery> avcDiscovery) {
    deps_.avcDiscovery = std::move(avcDiscovery);
}

void ControllerCore::SetFCPResponseRouter(
    std::shared_ptr<Protocols::AVC::FCPResponseRouter> fcpResponseRouter) {
    deps_.fcpResponseRouter = std::move(fcpResponseRouter);
}

Protocols::SBP2::AddressSpaceManager* ControllerCore::GetSbp2AddressSpaceManager() const {
    return deps_.sbp2AddressSpaceManager.get();
}

void ControllerCore::SetSbp2AddressSpaceManager(
    std::shared_ptr<Protocols::SBP2::AddressSpaceManager> sbp2AddressSpaceManager) {
    deps_.sbp2AddressSpaceManager = std::move(sbp2AddressSpaceManager);
}

IRM::IRMClient* ControllerCore::GetIRMClient() const { return deps_.irmClient.get(); }

void ControllerCore::SetIRMClient(std::shared_ptr<IRM::IRMClient> client) {
    deps_.irmClient = std::move(client);
}

CMP::CMPClient* ControllerCore::GetCMPClient() const { return deps_.cmpClient.get(); }

void ControllerCore::SetCMPClient(std::shared_ptr<CMP::CMPClient> client) {
    deps_.cmpClient = std::move(client);
}

// Diagnostic accessors for UserClient handlers
HardwareInterface* ControllerCore::GetHardware() const { return deps_.hardware.get(); }

BusResetCoordinator* ControllerCore::GetBusResetCoordinator() const {
    return deps_.busReset.get();
}

BusManager* ControllerCore::GetBusManager() const { return deps_.busManager.get(); }

// Phase 2: Interface facade accessors
Async::IFireWireBus& ControllerCore::Bus() {
    if (!busImpl_) {
        ASFW_LOG(Controller, "❌ CRITICAL: Bus() called before facade initialized");
        __builtin_trap();
    }
    return *busImpl_;
}

Async::IFireWireBus& ControllerCore::Bus() const {
    if (!busImpl_) {
        ASFW_LOG(Controller, "❌ CRITICAL: Bus() called before facade initialized");
        __builtin_trap();
    }
    return *busImpl_;
}

Shared::IDMAMemory& ControllerCore::DMA() {
    if (!dmaImpl_ && deps_.asyncController) {
        auto* dmaManager = deps_.asyncController->GetDMAManager();
        if (dmaManager != nullptr) {
            dmaImpl_ = std::make_unique<Async::DMAMemoryImpl>(*dmaManager);
            ASFW_LOG(Controller, "✅ DMAMemoryImpl facade created");
        } else {
            ASFW_LOG(Controller, "❌ CRITICAL: DMA() called before DMAMemoryManager initialized");
            __builtin_trap();
        }
    }
    return *dmaImpl_;
}

Async::IAsyncControllerPort& ControllerCore::AsyncSubsystem() const {
    if (!deps_.asyncController) {
        ASFW_LOG(Controller, "❌ CRITICAL: AsyncSubsystem() called with null dependency");
        __builtin_trap();
    }
    return *deps_.asyncController;
}

} // namespace ASFW::Driver
