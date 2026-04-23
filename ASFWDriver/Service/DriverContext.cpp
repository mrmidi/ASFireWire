#include "DriverContext.hpp"

#include <net.mrmidi.ASFW.ASFWDriver/ASFWDriver.h>

#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

#include "../Async/AsyncSubsystem.hpp"
#include "../Async/Interfaces/IFireWireBus.hpp"
#include "../Async/PacketHelpers.hpp"
#include "../Async/ResponseCode.hpp"
#include "../Async/Tx/ResponseSender.hpp"
#include "../Audio/AudioCoordinator.hpp"
#include "../Bus/BusManager.hpp"
#include "../Bus/BusResetCoordinator.hpp"
#include "../Bus/SelfIDCapture.hpp"
#include "../Bus/TopologyManager.hpp"
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
    deps.fcpResponseRouter.reset(); // Clean up FCP router
    deps.sbp2AddressSpaceManager.reset();
    deps.avcDiscovery.reset();      // Clean up AV/C discovery
    deps.irmClient.reset();         // Clean up IRM client
    deps.asyncController.reset();
    deps.asyncSubsystem.reset(); // Stop and cleanup asyncSubsystem
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

    if (!ctx.audioCoordinator && d.deviceManager && d.deviceRegistry && d.hardware) {
        ctx.audioCoordinator = std::make_shared<ASFW::Audio::AudioCoordinator>(
            driver, *d.deviceManager, *d.deviceRegistry, ctx.isoch, *d.hardware);
        ASFW_LOG(Controller, "[Controller] ✅ AudioCoordinator initialized");
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

    if (!d.asyncSubsystem) {
        return;
    }

    auto* router = d.asyncSubsystem->GetPacketRouter();
    if (!router) {
        return;
    }

    auto* sbp2Manager = d.sbp2AddressSpaceManager.get();
    auto* fcpRouter = d.fcpResponseRouter.get();
    auto* responder = router->GetResponseSender();

    router->RegisterRequestHandler(
        0x0,
        [sbp2Manager](const ASFW::Async::ARPacketView& packet) {
            if (!sbp2Manager || packet.header.size() < 16) {
                return ASFW::Async::ResponseCode::AddressError;
            }

            const uint64_t destOffset = ASFW::Async::ExtractDestOffset(packet.header);
            const auto quadletData = std::span<const uint8_t>(packet.header.data() + 12, 4);
            return sbp2Manager->ApplyRemoteWrite(destOffset, quadletData);
        });

    router->RegisterRequestHandler(
        0x1,
        [sbp2Manager, fcpRouter](const ASFW::Async::ARPacketView& packet) {
            if (sbp2Manager && packet.header.size() >= 16 && !packet.payload.empty()) {
                const uint64_t destOffset = ASFW::Async::ExtractDestOffset(packet.header);
                const auto sbp2Result =
                    sbp2Manager->ApplyRemoteWrite(destOffset, packet.payload);
                if (sbp2Result != ASFW::Async::ResponseCode::AddressError) {
                    return sbp2Result;
                }
            }

            if (fcpRouter) {
                const ASFW::Protocols::Ports::BlockWriteRequestView request{
                    .sourceID = packet.sourceID,
                    .destOffset = ASFW::Async::ExtractDestOffset(packet.header),
                    .payload = packet.payload,
                };
                const auto disposition = fcpRouter->RouteBlockWrite(request);
                if (disposition == ASFW::Protocols::Ports::BlockWriteDisposition::kAddressError) {
                    return ASFW::Async::ResponseCode::AddressError;
                }
                return ASFW::Async::ResponseCode::Complete;
            }

            return ASFW::Async::ResponseCode::AddressError;
        });

    router->RegisterRequestHandler(
        0x4,
        [sbp2Manager, responder](const ASFW::Async::ARPacketView& packet) {
            uint32_t quadlet = 0;
            auto result = ASFW::Async::ResponseCode::AddressError;

            if (sbp2Manager && packet.header.size() >= 12) {
                const uint64_t destOffset = ASFW::Async::ExtractDestOffset(packet.header);
                result = sbp2Manager->ReadQuadlet(destOffset, &quadlet);
            }

            if (responder) {
                responder->SendReadQuadletResponse(packet, result, quadlet);
            }
            return ASFW::Async::ResponseCode::NoResponse;
        });

    router->RegisterRequestHandler(
        0x5,
        [sbp2Manager, responder](const ASFW::Async::ARPacketView& packet) {
            auto result = ASFW::Async::ResponseCode::AddressError;
            ASFW::Protocols::SBP2::AddressSpaceManager::ReadSlice slice{};

            if (sbp2Manager && packet.header.size() >= 16) {
                const uint64_t destOffset = ASFW::Async::ExtractDestOffset(packet.header);
                const auto readLength =
                    static_cast<uint32_t>(ASFW::Async::ExtractDataLength(packet.header));
                if (readLength > 0) {
                    result = sbp2Manager->ResolveReadSlice(destOffset, readLength, &slice);
                } else {
                    result = ASFW::Async::ResponseCode::DataError;
                }
            }

            if (responder) {
                if (result == ASFW::Async::ResponseCode::Complete) {
                    responder->SendReadBlockResponse(
                        packet, result, slice.payloadDeviceAddress, slice.payloadLength);
                } else {
                    responder->SendReadBlockResponse(packet, result, 0, 0);
                }
            }
            return ASFW::Async::ResponseCode::NoResponse;
        });

    ASFW_LOG(Controller,
             "[Controller] PacketRouter wired for SBP2 address spaces (tCode 0x0/0x1/0x4/0x5)");
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
