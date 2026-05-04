#pragma once

#include <DriverKit/IOReturn.h>
#include <memory>
#include <string_view>

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

namespace ASFW::Shared {
class IDMAMemory;
}

namespace ASFW::Async {
class AsyncSubsystem;
class IAsyncControllerPort;
class IFireWireBus;
class FireWireBusImpl;
class DMAMemoryImpl;
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
class ControllerCore {
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

        std::shared_ptr<ASFW::IRM::IRMClient> irmClient;

        std::shared_ptr<ASFW::CMP::CMPClient> cmpClient;
    };

    ControllerCore(ControllerConfig config, Dependencies deps);
    ~ControllerCore();

    kern_return_t Start(IOService* provider);
    void Stop();

    void HandleInterrupt(const InterruptSnapshot& snapshot);

    const ControllerStateMachine& StateMachine() const;
    MetricsSink& Metrics() const;
    std::optional<TopologySnapshot> LatestTopology() const;

    Async::IFireWireBus& Bus();
    Async::IFireWireBus& Bus() const;
    Shared::IDMAMemory& DMA();

    Async::IAsyncControllerPort& AsyncSubsystem() const;

    // Diagnostic accessors for UserClient handlers
    HardwareInterface* GetHardware() const;
    BusResetCoordinator* GetBusResetCoordinator() const;
    BusManager* GetBusManager() const;

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

  private:
    void LogBuildBanner() const;
    kern_return_t InitializeBusResetAndDiscovery();
    kern_return_t PerformSoftReset() const;
    kern_return_t InitialiseHardware(IOService* provider);
    kern_return_t EnableInterruptsAndStartBus();
    kern_return_t StageConfigROM(uint32_t busOptions, uint32_t guidHi, uint32_t guidLo) const;
    void DiagnoseUnrecoverableError() const;
    void HandleCycle64Seconds(); // Called on cycle64Seconds interrupt to extend 7-bit seconds

    void OnTopologyReady(const TopologySnapshot& snapshot);
    void OnDiscoveryScanComplete(Discovery::Generation gen,
                                 const std::vector<Discovery::ConfigROM>& roms,
                                 bool hadBusyNodes) const;

    ControllerConfig config_;
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
};

} // namespace ASFW::Driver
