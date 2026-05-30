#pragma once

#include <DriverKit/IOReturn.h>
#include <functional>
#include <memory>
#include <string_view>

#include "../Bus/Role/CycleObserver.hpp"
#include "../Bus/Role/RoleCoordinator.hpp"
#include "../Bus/BusManager/BusManagerRuntimeState.hpp"
#include "../Bus/BusManager/BusManagerElectionDriver.hpp"
#include "../Bus/BusManager/LocalIRMResourceController.hpp"
#include "../Bus/BusManager/BusManagerPolicyCoordinator.hpp"
#include "../Discovery/DiscoveryTypes.hpp" // For Discovery::Generation
#include "ControllerConfig.hpp"
#include "ControllerTypes.hpp"

class IOService;

namespace ASFW::Driver {

class HardwareInterface;
class InterruptManager;
class ControllerStateMachine;
class Scheduler;
class ConfigROMBuilder;
class ConfigROMStager;
class SelfIDCapture;
class TopologyManager;
class BusResetCoordinator;
class BusManager;
class MetricsSink;

} // namespace ASFW::Driver

namespace ASFW::Bus {
class IRootStatus;
class ICycleMasterControl;
class IBusResetTrigger;
class CSRResponder;
class TopologyMapService;
class BusManagerElectionDriver;
class BusManagerPolicyCoordinator;
} // namespace ASFW::Bus

namespace ASFW::Shared {
class IDMAMemory;
}

namespace ASFW::Async {
class AsyncSubsystem;
class IAsyncControllerPort;
class IFireWireBus;
class FireWireBusImpl;
class DMAMemoryImpl;
class LocalRequestDispatch;
} // namespace ASFW::Async

namespace ASFW::Discovery {
class SpeedPolicy;
class ConfigROMStore;
class DeviceRegistry;
class ROMScanner;
struct ROMScanRequest;
class DeviceManager;
class IDeviceManager;
class IUnitRegistry;
} // namespace ASFW::Discovery

namespace ASFW::Protocols::AVC {
class AVCDiscovery;
class IAVCDiscovery;
class FCPResponseRouter;
} // namespace ASFW::Protocols::AVC

namespace ASFW::Protocols::SBP2 {
class AddressSpaceManager;
}

namespace ASFW::IRM {
class IRMClient;
}
namespace ASFW::CMP {
class CMPClient;
}

namespace ASFW::Driver {

// Central orchestrator that wires together hardware access, interrupt routing,
// bus reset sequencing, and topology publication.
class ControllerCore final : private Role::IPhyConfigReset,
                             private Role::IRemoteCsrWriter,
                             private Role::IContenderControl,
                             public ASFW::Bus::BusManagerElectionDriver::IBMRoleEvents,
                             public ASFW::Bus::IBMPolicyExecutor,
                             public std::enable_shared_from_this<ControllerCore> {
  public:
    struct Dependencies {
        std::shared_ptr<HardwareInterface> hardware;
        std::shared_ptr<InterruptManager> interrupts;
        std::shared_ptr<Scheduler> scheduler;
        std::shared_ptr<ConfigROMBuilder> configRom;
        std::shared_ptr<ConfigROMStager> configRomStager;
        std::shared_ptr<SelfIDCapture> selfId;
        std::shared_ptr<TopologyManager> topology;
        std::shared_ptr<BusResetCoordinator> busReset;
        std::shared_ptr<BusManager> busManager;
        std::shared_ptr<MetricsSink> metrics;
        std::shared_ptr<ControllerStateMachine> stateMachine;
        std::shared_ptr<ASFW::Async::AsyncSubsystem> asyncSubsystem;
        std::shared_ptr<ASFW::Async::IAsyncControllerPort> asyncController;

        std::shared_ptr<ASFW::Discovery::SpeedPolicy> speedPolicy;
        std::shared_ptr<ASFW::Discovery::ConfigROMStore> romStore;
        std::shared_ptr<ASFW::Discovery::DeviceRegistry> deviceRegistry;
        std::shared_ptr<ASFW::Discovery::ROMScanner> romScanner;
        std::shared_ptr<ASFW::Discovery::DeviceManager> deviceManager;

        std::shared_ptr<ASFW::Protocols::AVC::AVCDiscovery> avcDiscovery;
        std::shared_ptr<ASFW::Protocols::AVC::FCPResponseRouter> fcpResponseRouter;
        std::shared_ptr<ASFW::Protocols::SBP2::AddressSpaceManager> sbp2AddressSpaceManager;

        // FW-19: local software CSR responder (STATE_SET/CLEAR, BROADCAST_CHANNEL,
        // TOPOLOGY_MAP) plus its hardware adapters for root status / cycle master.
        std::shared_ptr<ASFW::Bus::IRootStatus> csrRootStatus;
        std::shared_ptr<ASFW::Bus::ICycleMasterControl> csrCycleMasterControl;
        std::shared_ptr<ASFW::Bus::IBusResetTrigger> csrResetTrigger;
        std::shared_ptr<ASFW::Bus::CSRResponder> csrResponder;
        std::shared_ptr<ASFW::Bus::TopologyMapService> topologyMapService;

        // FW-19: single owner of inbound local request routing (CSR/SBP-2/FCP/DICE).
        std::shared_ptr<ASFW::Async::LocalRequestDispatch> localRequestDispatch;

        std::shared_ptr<ASFW::IRM::IRMClient> irmClient;

        std::shared_ptr<ASFW::CMP::CMPClient> cmpClient;
        std::shared_ptr<ASFW::Bus::BusManagerElectionDriver> busManagerElectionDriver;
        std::function<void()> cycleInconsistentCallback;
    };

    ControllerCore(ControllerConfig config, RolePolicy initialPolicy, Dependencies deps);
    ~ControllerCore();

    kern_return_t Start(IOService* provider);
    void Stop();

    void HandleInterrupt(const InterruptSnapshot& snapshot);

    const ControllerStateMachine& StateMachine() const;
    MetricsSink& Metrics() const;
    std::optional<TopologySnapshot> LatestTopology() const;
    [[nodiscard]] const ControllerConfig& GetConfig() const noexcept { return config_; }
    [[nodiscard]] const RolePolicy& GetRolePolicy() const noexcept { return rolePolicy_; }

    // Runtime role-policy switch (FW-21/FW-22). Updates the stored policy, re-seeds
    // the RoleCoordinator gate, and — if the controller is already running —
    // re-stages the local Config ROM (BIB capabilities) and triggers a bus reset
    // so peers re-read it. Used by Start() with the initial policy and by future
    // runtime callers (UserClient) to flip role mode live.
    kern_return_t ApplyRolePolicy(const RolePolicy& policy);

    Async::IFireWireBus& Bus();
    Async::IFireWireBus& Bus() const;
    Shared::IDMAMemory& DMA();

    Async::IAsyncControllerPort& AsyncSubsystem() const;

    // Diagnostic accessors for UserClient handlers
    HardwareInterface* GetHardware() const;
    BusResetCoordinator* GetBusResetCoordinator() const;
    BusManager* GetBusManager() const;
    const Role::RoleCoordinator& GetRoleCoordinator() const { return roleCoordinator_; }

    Discovery::ConfigROMStore* GetConfigROMStore() const;
    Discovery::ROMScanner* GetROMScanner() const;
    void AttachROMScanner(std::shared_ptr<Discovery::ROMScanner> romScanner);
    [[nodiscard]] bool StartDiscoveryScan(const Discovery::ROMScanRequest& request);

    Discovery::IDeviceManager* GetDeviceManager() const;
    Discovery::IUnitRegistry* GetUnitRegistry() const;
    Discovery::DeviceRegistry* GetDeviceRegistry() const;

    Protocols::AVC::IAVCDiscovery* GetAVCDiscovery() const;
    void SetAVCDiscovery(std::shared_ptr<Protocols::AVC::AVCDiscovery> avcDiscovery);
    void SetFCPResponseRouter(std::shared_ptr<Protocols::AVC::FCPResponseRouter> fcpResponseRouter);
    Protocols::SBP2::AddressSpaceManager* GetSbp2AddressSpaceManager() const;
    void SetSbp2AddressSpaceManager(
        std::shared_ptr<Protocols::SBP2::AddressSpaceManager> sbp2AddressSpaceManager);

    IRM::IRMClient* GetIRMClient() const;
    void SetIRMClient(std::shared_ptr<IRM::IRMClient> client);

    CMP::CMPClient* GetCMPClient() const;
    void SetCMPClient(std::shared_ptr<CMP::CMPClient> client);

    Bus::BusManagerElectionDriver* GetBusManagerElectionDriver() const;
    void SetBusManagerElectionDriver(std::shared_ptr<Bus::BusManagerElectionDriver> driver);

    const Bus::BusManagerRuntimeState& GetBusManagerRuntimeState() const {
        SyncBusManagerRuntimeState();
        return bmState_;
    }
    Bus::BusManagerRuntimeState& GetBusManagerRuntimeState() {
        SyncBusManagerRuntimeState();
        return bmState_;
    }

    ASFW::Bus::TopologyMapService* GetTopologyMapService() const { return deps_.topologyMapService.get(); }
    ASFW::Bus::LocalIRMResourceController* GetLocalIRMResourceController() const { return localIrmController_.get(); }

  private:
    void LogBuildBanner() const;
    kern_return_t InitializeBusResetAndDiscovery();
    kern_return_t PerformSoftReset() const;
    kern_return_t InitialiseHardware(IOService* provider);
    kern_return_t EnableInterruptsAndStartBus();
    kern_return_t StageConfigROM(uint32_t busOptions, uint32_t guidHi, uint32_t guidLo) const;
    void LogInterruptContext(const InterruptSnapshot& snapshot,
                             uint32_t rawEvents,
                             uint32_t currentMask,
                             uint32_t events) const;
    void HandleFaultInterrupts(uint32_t events);
    void NotifyBusResetCoordinator(uint32_t events, uint64_t timestamp) const;
    void DispatchAsyncInterrupts(uint32_t events) const;
    void LogBusResetCompletionEvents(uint32_t events, uint64_t timestamp) const;
    [[nodiscard]] static uint32_t FaultAckMask(uint32_t events) noexcept;
    void DiagnoseUnrecoverableError() const;
    void HandleCycle64Seconds(); // Called on cycle64Seconds interrupt to extend 7-bit seconds
    void EvaluateBusManagerPolicy() noexcept;

    void OnTopologyReady(const TopologySnapshot& snapshot);
    void BeginRootCapabilityEvidence(const TopologySnapshot& snapshot, uint8_t localNodeId);
    void OnRootCapabilityProbe(Role::RootCapabilityEvidence evidence);
    void StartRootCycleLostWindow(uint32_t generation);
    void CompleteRootCycleLostWindow(uint32_t generation, uint32_t epoch, bool cycleLost);
    void PublishRootCapabilityEvidence();
    void OnDiscoveryScanComplete(Discovery::Generation gen,
                                 const std::vector<Discovery::ConfigROM>& roms,
                                 bool hadBusyNodes) const;
    void ForceRootAndReset(uint8_t targetRoot, Role::RoleResetFlavor flavor, uint8_t gapCount,
                           uint32_t generation) override;
    void EnableRemoteCycleMaster(uint8_t rootNodeId, uint32_t generation) override;
    void EnableLocalCycleMaster(uint32_t generation) override;
    void ClearLocalContenderAndDelegate(uint8_t targetRoot, uint32_t generation) override;

    // ASFW::Bus::BusManagerElectionDriver::IBMRoleEvents implementation
    void OnLocalWonBM(uint32_t generation, uint8_t localNodeId) override;
    void OnRemoteBM(uint32_t generation, uint8_t remoteNodeId) override;
    void OnBMElectionFailed(uint32_t generation, ASFW::Async::AsyncStatus status) override;

    // ASFW::Bus::IBMPolicyExecutor implementation
    void SendRemoteCmstr(uint8_t rootNodeId, uint32_t generation) override;
    void HandleRemoteCmstrCallback(uint32_t generation, uint8_t rootNodeId, ASFW::Async::AsyncStatus status);
    void SyncBusManagerRuntimeState() const noexcept;

    ControllerConfig config_;
    RolePolicy rolePolicy_; // runtime-mutable; see ApplyRolePolicy()
    Dependencies deps_;
    bool running_{false};
    bool hardwareAttached_{false};
    bool hardwareInitialised_{false};
    bool busTimeRunning_{false};
    uint32_t ohciVersion_{0};
    bool phyProgramSupported_{false};
    bool phyConfigOk_{false};

    // Extended 32-bit bus cycle time (OHCI cycle timer only has 7-bit seconds field)
    // Updated on cycle64Seconds interrupt (every 64 seconds when 7-bit counter rolls over)
    // Per Apple's handleCycle64Int: extends 7-bit seconds to full 32-bit counter
    uint32_t busCycleTime_{0};

    std::unique_ptr<Async::FireWireBusImpl> busImpl_;
    std::unique_ptr<Async::DMAMemoryImpl> dmaImpl_;

    // FW-6/FW-7: role / cycle-master policy. Fed from OnTopologyReady (topology)
    // and HandleFaultInterrupts (cycle evidence), both on the single-threaded
    // controller queue. Behavior-neutral for now: the skeleton policy returns
    // only None/Defer and executors are null, so no hardware action is taken
    // until FW-9 wires the executors and fills in EvaluateRolePolicy.
    Role::RoleCoordinator roleCoordinator_{};
    Role::CycleObserver cycleObserver_{};
    uint32_t currentGeneration_{0};
    Role::RootCapabilityEvidence currentRootEvidence_{};
    bool haveRootEvidence_{false};
    bool cycleLostWindowActive_{false};
    uint32_t cycleLostWindowEpoch_{0};

    mutable Bus::BusManagerRuntimeState bmState_{};
    std::unique_ptr<Bus::BusManagerPolicyCoordinator> bmPolicyCoordinator_;
    std::unique_ptr<Bus::LocalIRMResourceController> localIrmController_;
};

} // namespace ASFW::Driver
