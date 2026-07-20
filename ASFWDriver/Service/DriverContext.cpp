#include "DriverContext.hpp"

#include <net.mrmidi.ASFW.ASFWDriver/ASFWDriver.h>

#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

#include "../Async/AsyncSubsystem.hpp"
#include "../Async/Interfaces/IFireWireBus.hpp"
#include "../Async/PacketHelpers.hpp"
#include "../Async/ResponseCode.hpp"
#include "../Async/Tx/ResponseSender.hpp"
#include "../Audio/Core/AudioCoordinator.hpp"
#include "../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../Bus/BusManager.hpp"
#include "../Bus/BusResetCoordinator.hpp"
#include "../Bus/SelfIDCapture.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../Bus/CSR/BroadcastChannelCSR.hpp"
#include "../Bus/CSR/TopologyMapService.hpp"
#include "../Bus/BusManager/BusManagerElectionDriver.hpp"
#include "../ConfigROM/ConfigROMBuilder.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "../ConfigROM/ConfigROMStore.hpp"
#include "../Controller/ControllerStateMachine.hpp"
#include "../Diagnostics/MetricsSink.hpp"
#include "../Discovery/DeviceManager.hpp"
#include "../Discovery/DeviceRegistry.hpp"
#include "../Discovery/SpeedPolicy.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Hardware/InterruptManager.hpp"
#include "../Logging/Logging.hpp"
#include "../Protocols/AVC/FCPResponseRouter.hpp"
#include "../Protocols/AVC/AVCDiscovery.hpp"
#include "../Protocols/SBP2/AddressSpaceManager.hpp"
#include "../Protocols/SBP2/Session/DriverKitSessionScheduler.hpp"
#include "../Protocols/SBP2/Session/SessionRegistry.hpp"
#include "../SCSIController/SBP2BridgeHub.hpp"
#include "../SCSIController/SBP2TargetBridge.hpp"
#include "../Scheduling/Scheduler.hpp"

void ServiceContext::DisarmProviderNotifications() {
#ifndef ASFW_HOST_TEST
    if (providerNotifications) {
        (void)providerNotifications->SetEnableWithCompletion(false, nullptr);
        // Do not call Cancel(nullptr) here. DriverKit dispatches cancel asynchronously,
        // and releasing the source before that block runs can crash in Cancel_Impl.
    }
    providerNotifications.reset();
    providerNotificationAction.reset();
#endif
}

void ServiceContext::Reset(ResetMode mode) {
    stopping.store(true, std::memory_order_release);
    if (deps.asyncSubsystem) {
        deps.asyncSubsystem->BeginQuiesce();
    }
    if (audioCoordinator) {
        audioCoordinator->BeginTeardown();
    }
    if (deps.avcDiscovery) {
        deps.avcDiscovery->Shutdown();
    }
    // Unpublish + shut down the HBA bridge while the session registry and bus
    // are still alive: Shutdown() releases the SBP-2 session (logout on the
    // wire) and aborts in-flight/queued HBA tasks back to the SCSI layer.
    ASFW::Protocols::SBP2::SBP2BridgeHub::Clear();
    if (sbp2Bridge) {
        sbp2Bridge->Shutdown();
        sbp2Bridge.reset();
    }
    isoch.StopAll();
    controller.reset();
    audioCoordinator.reset();
    // Tear down the runtime audio protocols while the services they were built from
    // (bus/hardware/IRM) are still alive. controller.reset() above dropped the controller's
    // copy of this shared_ptr, so the deps copy is the last owner; resetting it here lets the
    // protocol destructors run before the bus/IRM teardown below.
    deps.audioRuntimeRegistry.reset();
    deps.hardware.reset();
    deps.busReset.reset();
    deps.busManager.reset();
    deps.selfId.reset();
    deps.scheduler.reset();
    deps.metrics.reset();
    deps.stateMachine.reset();
    deps.configRom.reset();
    deps.configRomStager.reset();
    if (mode == ResetMode::Full) {
        deps.interrupts.reset(); // ~InterruptManager cancels the dispatch source
    }
    deps.topology.reset();
    deps.topologyMapService.reset();
    deps.busManagerElectionDriver.reset();
    deps.fcpResponseRouter.reset(); // Clean up FCP router
    deps.sbp2SessionRegistry.reset();
    deps.sbp2SessionScheduler.reset();
    deps.sbp2AddressSpaceManager.reset();
    deps.avcDiscovery.reset();      // Clean up AV/C discovery
    deps.irmClient.reset();         // Clean up IRM client
    deps.asyncController.reset();
    deps.asyncSubsystem.reset(); // Stop and cleanup asyncSubsystem
    deps.cycleInconsistentCallback = {};
    statusPublisher.Reset();
    watchdog.Reset();
    DisarmProviderNotifications();
    if (mode == ResetMode::Full) {
        workQueue.reset();
        interruptAction.reset();
    }
}

namespace ASFW::Driver {

void DriverWiring::EnsureDeps(ASFWDriver* driver, ::ServiceContext& ctx) {
    auto& d = ctx.deps;
    if (!d.hardware) {
        d.hardware = std::make_shared<HardwareInterface>();
    }
    if (!d.busReset) {
        d.busReset = std::make_shared<BusResetCoordinator>();
    }
    if (!d.selfId) {
        d.selfId = std::make_shared<SelfIDCapture>();
    }
    if (!d.scheduler) {
        d.scheduler = std::make_shared<Scheduler>();
    }
    if (!d.metrics) {
        d.metrics = std::make_shared<MetricsSink>();
    }
    if (!d.stateMachine) {
        d.stateMachine = std::make_shared<ControllerStateMachine>();
    }
    if (!d.configRom) {
        d.configRom = std::make_shared<ConfigROMBuilder>();
    }
    if (!d.configRomStager) {
        d.configRomStager = std::make_shared<ConfigROMStager>();
    }
    if (!d.interrupts) {
        d.interrupts = std::make_shared<InterruptManager>();
    }
    if (!d.topology) {
        d.topology = std::make_shared<TopologyManager>();
    }
    if (!d.busManager) {
        d.busManager = std::make_shared<BusManager>();
    }
    if (!d.broadcastChannel) {
        d.broadcastChannel = std::make_shared<ASFW::Bus::BroadcastChannelCSR>();
    }
    if (!d.topologyMapService && d.hardware) {
        d.topologyMapService = std::make_shared<ASFW::Bus::TopologyMapService>(d.hardware.get());
    }

    if (!d.asyncSubsystem) {
        d.asyncSubsystem = std::make_shared<ASFW::Async::AsyncSubsystem>();
    }
    if (!d.asyncController && d.asyncSubsystem) {
        d.asyncController =
            std::static_pointer_cast<ASFW::Async::IAsyncControllerPort>(d.asyncSubsystem);
    }

    if (!d.speedPolicy) {
        d.speedPolicy = std::make_shared<ASFW::Discovery::SpeedPolicy>();
    }
    if (!d.romStore) {
        d.romStore = std::make_shared<ASFW::Discovery::ConfigROMStore>();
    }
    if (!d.deviceRegistry) {
        d.deviceRegistry = std::make_shared<ASFW::Discovery::DeviceRegistry>();
    }
    if (!d.deviceManager) {
        d.deviceManager = std::make_shared<ASFW::Discovery::DeviceManager>();
    }

    // Runtime owner of device-specific IDeviceProtocol instances. Constructed here,
    // before AudioCoordinator and ControllerCore, so both can hold the same instance:
    // the controller triggers creation from its discovery path; the Audio layer reads it.
    if (!d.audioRuntimeRegistry) {
        d.audioRuntimeRegistry = std::make_shared<ASFW::Audio::AudioRuntimeRegistry>();
    }

    if (!ctx.audioCoordinator && d.deviceManager && d.deviceRegistry && d.hardware &&
        d.audioRuntimeRegistry) {
        ctx.audioCoordinator = std::make_shared<ASFW::Audio::AudioCoordinator>(
            driver, *d.deviceManager, *d.deviceRegistry, *d.audioRuntimeRegistry, ctx.isoch,
            *d.hardware);
        ASFW_LOG(Controller, "[Controller] ✅ AudioCoordinator initialized");
    }

    if (ctx.audioCoordinator) {
        std::weak_ptr<ASFW::Audio::AudioCoordinator> weakAudio = ctx.audioCoordinator;
        d.cycleInconsistentCallback = [weakAudio] {
            if (auto audio = weakAudio.lock()) {
                audio->HandleCycleInconsistent();
            }
        };
    } else {
        d.cycleInconsistentCallback = {};
    }

    // AV/C discovery wiring is done after ControllerCore is created so it can
    // depend only on IFireWireBus ports (ControllerCore::Bus()).
}

kern_return_t DriverWiring::EnsureSbp2Deps(ASFWDriver& service, ::ServiceContext& ctx) {
    auto& d = ctx.deps;

    if (!d.sbp2AddressSpaceManager && d.hardware) {
        d.sbp2AddressSpaceManager =
            std::make_shared<ASFW::Protocols::SBP2::AddressSpaceManager>(d.hardware.get());
        ASFW_LOG(Controller, "[Controller] SBP2 AddressSpaceManager initialized");
    }

    if (!d.sbp2SessionScheduler) {
        d.sbp2SessionScheduler =
            std::make_shared<ASFW::Protocols::SBP2::DriverKitSessionScheduler>();
        const auto kr = d.sbp2SessionScheduler->Prepare(service, ctx.workQueue);
        if (kr != kIOReturnSuccess) {
            d.sbp2SessionScheduler.reset();
            return kr;
        }
        ASFW_LOG(Controller, "[Controller] SBP2 session scheduler initialized");
    }

    if (!d.sbp2SessionRegistry && ctx.controller && d.sbp2AddressSpaceManager &&
        d.deviceManager && d.sbp2SessionScheduler) {
        auto& bus = ctx.controller->Bus();
        d.sbp2SessionRegistry = std::make_shared<ASFW::Protocols::SBP2::SessionRegistry>(
            bus, bus, *d.sbp2AddressSpaceManager, *d.deviceManager, *d.sbp2SessionScheduler,
            ctx.workQueue.get());
        if (d.busReset) {
            // Last-resort recovery for targets whose fetch engine wedges so hard
            // that even the LUN-reset management ORB is never fetched (LS-9000).
            // Long reset: the conservative flavor every device must honor.
            std::weak_ptr<BusResetCoordinator> weakReset = d.busReset;
            d.sbp2SessionRegistry->SetBusResetRequester([weakReset]() {
                if (auto coordinator = weakReset.lock()) {
                    coordinator->RequestUserReset(/*shortReset=*/false,
                                                  "SBP2 LUN-reset escalation");
                }
            });
        }
        ASFW_LOG(Controller, "[Controller] SBP2 SessionRegistry initialized");
    }

    if (ctx.controller) {
        ctx.controller->SetSbp2AddressSpaceManager(d.sbp2AddressSpaceManager);
        ctx.controller->SetSbp2SessionRegistry(d.sbp2SessionRegistry);
    }

    // Phase-1 HBA bridge: watches discovery for an SBP-2 unit, logs in, and
    // executes SCSI tasks handed over by ASFWSCSIController via SBP2BridgeHub.
    if (!ctx.sbp2Bridge && d.sbp2SessionRegistry && d.deviceManager && ctx.workQueue) {
        ctx.sbp2Bridge = std::make_shared<ASFW::Protocols::SBP2::SBP2TargetBridge>(
            d.sbp2SessionRegistry, *d.deviceManager, ctx.workQueue.get());
        ctx.sbp2Bridge->Start();
        ASFW::Protocols::SBP2::SBP2BridgeHub::Set(ctx.sbp2Bridge);
        ASFW_LOG(Controller, "[Controller] SBP2 target bridge initialized");
    }

    // Inbound local-request routing remains owned centrally by LocalRequestDispatch
    // (see WireLocalRequestDispatch). This helper owns the higher-level SBP-2
    // session dependencies that sit above the address-space manager.
    return kIOReturnSuccess;
}

kern_return_t DriverWiring::PrepareQueue(ASFWDriver& service, ::ServiceContext& ctx) {
    IODispatchQueue* q = nullptr;
    auto kr = service.CopyDispatchQueue("Default", &q);
    if (kr != kIOReturnSuccess || !q) {
        kr = service.CreateDefaultDispatchQueue(&q);
        if (kr != kIOReturnSuccess || !q)
            return kr != kIOReturnSuccess ? kr : kIOReturnError;
    }
    ctx.workQueue = OSSharedPtr(q, OSNoRetain);
    ctx.deps.scheduler->Bind(ctx.workQueue);
    return kIOReturnSuccess;
}

kern_return_t DriverWiring::PrepareInterrupts(ASFWDriver& service, IOService* provider,
                                              ::ServiceContext& ctx) {
    if (!provider) {
        return kIOReturnBadArgument;
    }

    auto intrMgr = ctx.deps.interrupts;
    if (!intrMgr) {
        return kIOReturnNoResources;
    }

    // MSI configuration happens once per provider lifetime, only before the
    // dispatch source exists. The source survives suspend/rebuild (see
    // ServiceContext::ResetMode): re-running ConfigureInterrupts or creating a
    // second source would re-register the same interrupt vector while the old
    // registration is still live, and the eventual double-unregister panics
    // the kernel on a shared interrupt controller.
    if (!intrMgr->HasSource()) {
        auto pci = OSDynamicCast(IOPCIDevice, provider);
        if (!pci) {
            return kIOReturnBadArgument;
        }

        auto status = pci->ConfigureInterrupts(
            kIOInterruptTypePCIMessagedX, 1, 1, 0);
        ASFW_LOG(Controller,
            "PrepareInterrupts: MSI-X ConfigureInterrupts -> 0x%08x",
            status);

        if (status != kIOReturnSuccess) {
            status = pci->ConfigureInterrupts(
                kIOInterruptTypePCIMessaged, 1, 1, 0);
            ASFW_LOG(Controller,
                "PrepareInterrupts: MSI ConfigureInterrupts -> 0x%08x",
                status);

            if (status != kIOReturnSuccess) {
                ASFW_LOG(Controller,
                    "PrepareInterrupts: MSI-X/MSI unavailable; "
                    "trying existing interrupt index 0");
            }
        }
    }

    if (!ctx.interruptAction) {
        OSAction* action = nullptr;
        auto kr = service.CreateActionInterruptOccurred(0, &action);
        if (kr != kIOReturnSuccess || !action)
            return kr != kIOReturnSuccess ? kr : kIOReturnError;
        ctx.interruptAction = OSSharedPtr(action, OSNoRetain);
    }

    auto kr = intrMgr->Initialise(
        provider, ctx.workQueue, ctx.interruptAction);
    ASFW_LOG(Controller,
        "PrepareInterrupts: InterruptManager::Initialise(index=0) "
        "-> 0x%08x",
        kr);
    if (kr != kIOReturnSuccess) {
        ctx.interruptAction.reset();
        return kr;
    }
    return kIOReturnSuccess;
}

kern_return_t DriverWiring::PrepareWatchdog(ASFWDriver& service, ::ServiceContext& ctx) {
    return ctx.watchdog.Prepare(service, ctx.workQueue);
}

void DriverWiring::CleanupStartFailure(::ServiceContext& ctx) {
    ctx.stopping.store(true, std::memory_order_release);
    if (ctx.deps.asyncSubsystem) {
        ctx.deps.asyncSubsystem->BeginQuiesce();
    }
    if (ctx.audioCoordinator) {
        ctx.audioCoordinator->BeginTeardown();
    }
    if (ctx.deps.avcDiscovery) {
        ctx.deps.avcDiscovery->Shutdown();
    }
    ASFW::Protocols::SBP2::SBP2BridgeHub::Clear();
    if (ctx.sbp2Bridge) {
        ctx.sbp2Bridge->Shutdown();
        ctx.sbp2Bridge.reset();
    }
    ctx.isoch.StopAll();
    if (ctx.deps.interrupts)
        ctx.deps.interrupts->Teardown();
    if (ctx.deps.selfId && ctx.deps.hardware)
        ctx.deps.selfId->Disarm(*ctx.deps.hardware);
    if (ctx.deps.selfId)
        ctx.deps.selfId->ReleaseBuffers();
    if (ctx.deps.configRomStager && ctx.deps.hardware)
        ctx.deps.configRomStager->Teardown(*ctx.deps.hardware);

    // Stop the async engine before ControllerCore::Stop closes the PCI BAR.
    if (ctx.deps.asyncSubsystem) {
        ctx.deps.asyncSubsystem->Stop();
    }
    if (ctx.controller) {
        ctx.controller->Stop();
        ctx.controller.reset();
    }
    if (ctx.deps.hardware)
        ctx.deps.hardware->Detach();
    ctx.interruptAction.reset();
    ctx.watchdog.Reset();
    ctx.DisarmProviderNotifications();
    ctx.workQueue.reset();
    ctx.statusPublisher.Reset();
}

} // namespace ASFW::Driver
