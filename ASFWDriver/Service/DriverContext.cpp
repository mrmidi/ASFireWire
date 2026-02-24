#include "DriverContext.hpp"

#include <net.mrmidi.ASFW.ASFWDriver/ASFWDriver.h>

#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

#include "../Async/AsyncSubsystem.hpp"
#include "../Async/ResponseCode.hpp"
#include "../Bus/BusManager.hpp"
#include "../Bus/BusResetCoordinator.hpp"
#include "../Bus/SelfIDCapture.hpp"
#include "../Bus/TopologyManager.hpp"
#include "../ConfigROM/ConfigROMBuilder.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "../Controller/ControllerStateMachine.hpp"
#include "../Diagnostics/MetricsSink.hpp"
#include "../ConfigROM/ConfigROMStore.hpp"
#include "../Discovery/DeviceManager.hpp"
#include "../Discovery/DeviceRegistry.hpp"
#include "../Discovery/SpeedPolicy.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Hardware/InterruptManager.hpp"
#include "../Audio/AudioCoordinator.hpp"
#include "../Logging/Logging.hpp"
#include "../Protocols/AVC/AVCDiscovery.hpp"
#include "../Protocols/AVC/FCPResponseRouter.hpp"
#include "../Scheduling/Scheduler.hpp"

void ServiceContext::Reset() {
    stopping.store(true, std::memory_order_release);
    controller.reset();
    audioCoordinator.reset();
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
    deps.fcpResponseRouter.reset();  // Clean up FCP router
    deps.avcDiscovery.reset();       // Clean up AV/C discovery
    deps.irmClient.reset();          // Clean up IRM client
    deps.asyncSubsystem.reset();     // Stop and cleanup asyncSubsystem
    statusPublisher.Reset();
    watchdog.Reset();
#ifndef ASFW_HOST_TEST
    if (providerNotifications) {
        providerNotifications->SetEnableWithCompletion(false, nullptr);
        providerNotifications->Cancel(nullptr);
    }
    providerNotifications.reset();
    providerNotificationAction.reset();
#endif
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

    if (!d.asyncSubsystem) {
        d.asyncSubsystem = std::make_shared<ASFW::Async::AsyncSubsystem>();
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

    if (!ctx.audioCoordinator && d.deviceManager && d.deviceRegistry && d.hardware) {
        ctx.audioCoordinator = std::make_shared<ASFW::Audio::AudioCoordinator>(
            driver,
            *d.deviceManager,
            *d.deviceRegistry,
            ctx.isoch,
            *d.hardware
        );
        ASFW_LOG(Controller, "[Controller] ✅ AudioCoordinator initialized");
    }

    if (!d.avcDiscovery && d.deviceManager && d.asyncSubsystem) {
        d.avcDiscovery = std::make_shared<ASFW::Protocols::AVC::AVCDiscovery>(
            driver,
            *d.deviceManager,
            *d.asyncSubsystem,
            ctx.audioCoordinator.get()
        );
        ASFW_LOG(Controller, "[Controller] ✅ AVCDiscovery initialized");
    }

    if (!d.fcpResponseRouter && d.avcDiscovery && d.asyncSubsystem) {
        d.fcpResponseRouter = std::make_shared<ASFW::Protocols::AVC::FCPResponseRouter>(
            *d.avcDiscovery,
            d.asyncSubsystem->GetGenerationTracker()
        );
        ASFW_LOG(Controller, "[Controller] ✅ FCPResponseRouter initialized");
    }

    if (d.fcpResponseRouter && d.asyncSubsystem) {
        if (auto* router = d.asyncSubsystem->GetPacketRouter()) {
            router->RegisterRequestHandler(
                0x1, // tCode for Block Write Request
                [fcpRouter = d.fcpResponseRouter.get()](const ASFW::Async::ARPacketView& packet) {
                    if (fcpRouter) {
                        return fcpRouter->RouteBlockWrite(packet);
                    }
                    return ASFW::Async::ResponseCode::NoResponse;
                }
            );
            ASFW_LOG(Controller, "[Controller] ✅ FCPResponseRouter wired to PacketRouter (tCode 0x1)");
        }
    }
}

kern_return_t DriverWiring::PrepareQueue(ASFWDriver& service, ::ServiceContext& ctx) {
    IODispatchQueue* q = nullptr;
    auto kr = service.CopyDispatchQueue("Default", &q);
    if (kr != kIOReturnSuccess || !q) {
        kr = service.CreateDefaultDispatchQueue(&q);
        if (kr != kIOReturnSuccess || !q) return kr != kIOReturnSuccess ? kr : kIOReturnError;
    }
    ctx.workQueue = OSSharedPtr(q, OSNoRetain);
    ctx.deps.scheduler->Bind(ctx.workQueue);
    return kIOReturnSuccess;
}

kern_return_t DriverWiring::PrepareInterrupts(ASFWDriver& service, IOService* provider, ::ServiceContext& ctx) {
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
    if (kr != kIOReturnSuccess || !action) return kr != kIOReturnSuccess ? kr : kIOReturnError;
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
    if (ctx.controller) { ctx.controller->Stop(); ctx.controller.reset(); }

    // CRITICAL: Stop asyncSubsystem BEFORE cancelling watchdog
    // This prevents the crash where watchdog fires after completion queue is deactivated
    if (ctx.deps.asyncSubsystem) {
        ctx.deps.asyncSubsystem->Stop();
    }

    if (ctx.deps.interrupts) ctx.deps.interrupts->Disable();
    if (ctx.deps.selfId && ctx.deps.hardware) ctx.deps.selfId->Disarm(*ctx.deps.hardware);
    if (ctx.deps.selfId) ctx.deps.selfId->ReleaseBuffers();
    if (ctx.deps.configRomStager && ctx.deps.hardware) ctx.deps.configRomStager->Teardown(*ctx.deps.hardware);
    if (ctx.deps.hardware) ctx.deps.hardware->Detach();
    ctx.interruptAction.reset();
    ctx.watchdog.Reset();
#ifndef ASFW_HOST_TEST
    if (ctx.providerNotifications) {
        ctx.providerNotifications->SetEnableWithCompletion(false, nullptr);
        ctx.providerNotifications->Cancel(nullptr);
    }
    ctx.providerNotifications.reset();
    ctx.providerNotificationAction.reset();
#endif
    ctx.workQueue.reset();
    ctx.statusPublisher.Reset();
}

} // namespace ASFW::Driver
