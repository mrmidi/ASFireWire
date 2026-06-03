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
#include "../Protocols/SBP2/AddressSpaceManager.hpp"
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

void ServiceContext::Reset() {
    stopping.store(true, std::memory_order_release);
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
    deps.interrupts.reset();
    deps.topology.reset();
    deps.topologyMapService.reset();
    deps.busManagerElectionDriver.reset();
    deps.fcpResponseRouter.reset(); // Clean up FCP router
    deps.sbp2AddressSpaceManager.reset();
    deps.avcDiscovery.reset();      // Clean up AV/C discovery
    deps.irmClient.reset();         // Clean up IRM client
    deps.asyncController.reset();
    deps.asyncSubsystem.reset(); // Stop and cleanup asyncSubsystem
    deps.cycleInconsistentCallback = {};
    statusPublisher.Reset();
    watchdog.Reset();
    DisarmProviderNotifications();
    workQueue.reset();
    interruptAction.reset();
    isoch.StopAll();
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

void DriverWiring::EnsureSbp2Deps(::ServiceContext& ctx) {
    auto& d = ctx.deps;

    if (!d.sbp2AddressSpaceManager && d.hardware) {
        d.sbp2AddressSpaceManager =
            std::make_shared<ASFW::Protocols::SBP2::AddressSpaceManager>(d.hardware.get());
        ASFW_LOG(Controller, "[Controller] SBP2 AddressSpaceManager initialized");
    }

    if (ctx.controller) {
        ctx.controller->SetSbp2AddressSpaceManager(d.sbp2AddressSpaceManager);
    }

    // Inbound request routing (tCodes 0x0/0x1/0x4/0x5) is owned centrally by
    // LocalRequestDispatch (see WireLocalRequestDispatch), which registers the
    // SBP-2 / CSR / FCP / DICE address handlers in one place. This function only
    // constructs the SBP-2 manager dependency.
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

    auto pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        return kIOReturnBadArgument;
    }

    auto status = pci->ConfigureInterrupts(kIOInterruptTypePCIMessagedX, 1, 1, 0);
    if (status != kIOReturnSuccess) {
        status = pci->ConfigureInterrupts(kIOInterruptTypePCIMessaged, 1, 1, 0);
        if (status != kIOReturnSuccess) {
            return status;
        }
    }

    OSAction* action = nullptr;
    auto kr = service.CreateActionInterruptOccurred(0, &action);
    if (kr != kIOReturnSuccess || !action)
        return kr != kIOReturnSuccess ? kr : kIOReturnError;
    ctx.interruptAction = OSSharedPtr(action, OSNoRetain);
    auto intrMgr = ctx.deps.interrupts;
    if (!intrMgr) {
        return kIOReturnNoResources;
    }

    kr = intrMgr->Initialise(provider, ctx.workQueue, ctx.interruptAction);
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
    if (ctx.controller) {
        ctx.controller->Stop();
        ctx.controller.reset();
    }

    // CRITICAL: Stop asyncSubsystem BEFORE cancelling watchdog
    // This prevents the crash where watchdog fires after completion queue is deactivated
    if (ctx.deps.asyncSubsystem) {
        ctx.deps.asyncSubsystem->Stop();
    }

    if (ctx.deps.interrupts)
        ctx.deps.interrupts->Disable();
    if (ctx.deps.selfId && ctx.deps.hardware)
        ctx.deps.selfId->Disarm(*ctx.deps.hardware);
    if (ctx.deps.selfId)
        ctx.deps.selfId->ReleaseBuffers();
    if (ctx.deps.configRomStager && ctx.deps.hardware)
        ctx.deps.configRomStager->Teardown(*ctx.deps.hardware);
    if (ctx.deps.hardware)
        ctx.deps.hardware->Detach();
    ctx.interruptAction.reset();
    ctx.watchdog.Reset();
    ctx.DisarmProviderNotifications();
    ctx.workQueue.reset();
    ctx.statusPublisher.Reset();
}

} // namespace ASFW::Driver
