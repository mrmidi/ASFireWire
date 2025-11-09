#pragma once

#include <DriverKit/IOReturn.h>
#include <memory>
#include <string_view>

#include "ControllerConfig.hpp"
#include "ControllerTypes.hpp"
#include "../Discovery/DiscoveryTypes.hpp"  // For Discovery::Generation

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
class MetricsSink;

} // namespace ASFW::Driver

namespace ASFW::Async {
class AsyncSubsystem;
}

namespace ASFW::Discovery {
class SpeedPolicy;
class ConfigROMStore;
class DeviceRegistry;
class ROMScanner;
}

namespace ASFW::Driver {

// Central orchestrator that wires together hardware access, interrupt routing,
// bus reset sequencing, and topology publication. Detailed responsibilities are
// captured in DRAFT.md §6.1.
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
        std::shared_ptr<MetricsSink> metrics;
        std::shared_ptr<ControllerStateMachine> stateMachine;
        std::shared_ptr<ASFW::Async::AsyncSubsystem> asyncSubsystem;
        
        // Discovery subsystem
        std::shared_ptr<ASFW::Discovery::SpeedPolicy> speedPolicy;
        std::shared_ptr<ASFW::Discovery::ConfigROMStore> romStore;
        std::shared_ptr<ASFW::Discovery::DeviceRegistry> deviceRegistry;
        std::shared_ptr<ASFW::Discovery::ROMScanner> romScanner;
    };

    ControllerCore(const ControllerConfig& config, Dependencies deps);
    ~ControllerCore();

    kern_return_t Start(IOService* provider);
    void Stop();

    void HandleInterrupt(const InterruptSnapshot& snapshot);

    const ControllerStateMachine& StateMachine() const;
    MetricsSink& Metrics();
    std::optional<TopologySnapshot> LatestTopology() const;

    // Discovery subsystem accessors
    Discovery::ConfigROMStore* GetConfigROMStore() const;
    Discovery::ROMScanner* GetROMScanner() const;

private:
    kern_return_t PerformSoftReset();
    kern_return_t InitialiseHardware(IOService* provider);
    kern_return_t EnableInterruptsAndStartBus();
    kern_return_t StageConfigROM(uint32_t busOptions, uint32_t guidHi, uint32_t guidLo);
    void DiagnoseUnrecoverableError();
    
    // Discovery integration
    void OnTopologyReady(const TopologySnapshot& snapshot);
    void ScheduleDiscoveryPoll(Discovery::Generation gen);
    void PollDiscovery(Discovery::Generation gen);
    void OnDiscoveryScanComplete(Discovery::Generation gen);

    ControllerConfig config_;
    Dependencies deps_;
    bool running_{false};
    bool hardwareAttached_{false};
    bool hardwareInitialised_{false};
    bool busTimeRunning_{false};  // Tracks if isochronous cycle timer is active
    uint32_t ohciVersion_{0};     // Hardware OHCI version (masked: 0x00FF00FF)
    bool phyProgramSupported_{false};
    bool phyConfigOk_{false};

    // Self-ID interrupt state tracking (Phase 3B)
    // OHCI generates TWO Self-ID complete interrupts: selfIDComplete (bit 16) and selfIDComplete2 (bit 15)
    // Must wait for BOTH before re-arming buffer to avoid UnrecoverableError during DMA
    bool selfIDComplete1Seen_{false};
    bool selfIDComplete2Seen_{false};
};

} // namespace ASFW::Driver
