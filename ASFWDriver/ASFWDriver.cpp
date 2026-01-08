//
//  ASFWDriver.cpp
//  ASFWDriver
//
//  Created by Alexander Shabelnikov on 21.09.2025.
//

#define _LIBCPP_NO_ABI_TAG 1
#include <DriverKit/DriverKit.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSDictionary.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <PCIDriverKit/IOPCIFamilyDefinitions.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include <net.mrmidi.ASFW.ASFWDriver/ASFWDriver.h> // generated from .iig
#include <net.mrmidi.ASFW.ASFWDriver/ASFWDriverUserClient.h> // generated from .iig

#include "Controller/ControllerTypes.hpp"
#include "Controller/ControllerCore.hpp"
#include "Controller/ControllerConfig.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Hardware/InterruptManager.hpp"
#include "Scheduling/Scheduler.hpp"
#include "Bus/BusResetCoordinator.hpp"
#include "Bus/SelfIDCapture.hpp"
#include "Bus/TopologyManager.hpp"
#include "Bus/BusManager.hpp"
#include "Diagnostics/MetricsSink.hpp"
#include "Controller/ControllerStateMachine.hpp"
#include "ConfigROM/ConfigROMBuilder.hpp"
#include "ConfigROM/ConfigROMStager.hpp"
#include "Logging/Logging.hpp"
#include "Logging/LogConfig.hpp"
#include "Hardware/OHCIConstants.hpp"
#include "Hardware/RegisterMap.hpp"
#include "Async/AsyncSubsystem.hpp"
#include "Async/ResponseCode.hpp"
#include "Shared/Memory/DMAMemoryManager.hpp"
#include "Discovery/SpeedPolicy.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Discovery/DeviceManager.hpp"
#include "ConfigROM/ConfigROMStore.hpp"
#include "ConfigROM/ROMScanner.hpp"
#include "ConfigROM/ROMReader.hpp"
#include "Protocols/AVC/AVCDiscovery.hpp"
#include "Protocols/AVC/FCPResponseRouter.hpp"
#include "IRM/IRMClient.hpp"
#include "Protocols/AVC/CMP/CMPClient.hpp"
#include "Async/Interfaces/IFireWireBus.hpp"
#include "Isoch/IsochReceiveContext.hpp"
#include "Isoch/Transmit/IsochTransmitContext.hpp"
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include "Async/DMAMemoryImpl.hpp"
#include "Isoch/Memory/IsochDMAMemoryManager.hpp"
#include "Discovery/FWDevice.hpp"
#include "Discovery/DeviceManager.hpp"

using namespace ASFW::Driver;

class ASFWDriverUserClient;

namespace {
constexpr uint64_t kAsyncWatchdogPeriodUsec = 1000; // 1 ms tick (hybrid: interrupt + timer backup)

uint64_t MicrosecondsToMachTicks(uint64_t usec) {
    static mach_timebase_info_data_t timebase{0, 0};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }

    const __uint128_t nanos = static_cast<__uint128_t>(usec) * 1000u;
    // ns = ticks * numer / denom  => ticks = (ns * denom) / numer
    const __uint128_t scaled = nanos * timebase.denom;
    return static_cast<uint64_t>(scaled / timebase.numer);
}
} // namespace

struct ServiceContext {
    ControllerCore::Dependencies deps;
    ControllerConfig config{}; // placeholder config
    std::shared_ptr<ControllerCore> controller;
    OSSharedPtr<IODispatchQueue> workQueue;
    OSSharedPtr<OSAction> interruptAction;
    OSSharedPtr<IOTimerDispatchSource> watchdogTimer;
    OSSharedPtr<OSAction> watchdogAction;
    OSSharedPtr<IOBufferMemoryDescriptor> statusMemory;
    OSSharedPtr<IOMemoryMap> statusMap;
    SharedStatusBlock* statusBlock{nullptr};
    std::atomic<uint64_t> statusSequence{0};
    std::atomic<bool> stopping{false};
    OSSharedPtr<ASFWDriverUserClient> statusListener;
    uint64_t lastAsyncCompletionMach{0};
    uint32_t asyncTimeoutCount{0};
    uint64_t watchdogTickCount{0};
    uint64_t watchdogLastTickUsec{0};
    void Reset() {
        stopping.store(true, std::memory_order_release);
        controller.reset();
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
        statusListener.reset();
        statusBlock = nullptr;
        statusMemory.reset();
        statusMap.reset();
        statusSequence.store(0);
        lastAsyncCompletionMach = 0;
        asyncTimeoutCount = 0;
        watchdogTickCount = 0;
        watchdogLastTickUsec = 0;
        watchdogTimer.reset();
        watchdogAction.reset();
        workQueue.reset();
        interruptAction.reset();
        if (isochReceiveContext) {
            isochReceiveContext->Stop();
            isochReceiveContext = nullptr; 
        }
        if (isochTransmitContext) {
            isochTransmitContext->Stop();
            isochTransmitContext.reset();
        }
    }
    
    OSSharedPtr<ASFW::Isoch::IsochReceiveContext> isochReceiveContext;
    uint32_t isochLogDivider{0};
    
    std::unique_ptr<ASFW::Isoch::IsochTransmitContext> isochTransmitContext;
    uint32_t itLogDivider{0};
};

namespace {

kern_return_t PrepareStatusBlock(ServiceContext& ctx) {
    if (ctx.statusBlock != nullptr) {
        return kIOReturnSuccess;
    }

    IOBufferMemoryDescriptor* rawBuffer = nullptr;
    auto kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                               sizeof(SharedStatusBlock),
                                               64,
                                               &rawBuffer);
    if (kr != kIOReturnSuccess || rawBuffer == nullptr) {
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }

    rawBuffer->SetLength(sizeof(SharedStatusBlock));
    ctx.statusMemory = OSSharedPtr(rawBuffer, OSNoRetain);

    IOMemoryMap* rawMap = nullptr;
    kr = rawBuffer->CreateMapping(0, 0, 0, 0, 0, &rawMap);
    if (kr != kIOReturnSuccess || rawMap == nullptr) {
        ctx.statusMemory.reset();
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }

    ctx.statusMap = OSSharedPtr(rawMap, OSNoRetain);
    ctx.statusBlock = reinterpret_cast<SharedStatusBlock*>(rawMap->GetAddress());
    if (!ctx.statusBlock) {
        ctx.statusMap.reset();
        ctx.statusMemory.reset();
        return kIOReturnNoMemory;
    }

    std::memset(ctx.statusBlock, 0, sizeof(SharedStatusBlock));
    ctx.statusBlock->version = SharedStatusBlock::kVersion;
    ctx.statusBlock->length = sizeof(SharedStatusBlock);
    ctx.statusBlock->sequence = 0;
    ctx.statusBlock->reason = static_cast<uint32_t>(SharedStatusReason::Boot);
    ctx.statusBlock->updateTimestamp = mach_absolute_time();
    return kIOReturnSuccess;
}

void PublishStatus(ServiceContext& ctx,
                   SharedStatusReason reason,
                   uint32_t detailMask = 0) {
    if (!ctx.statusBlock) {
        return;
    }

    SharedStatusBlock snapshot{};
    snapshot.version = SharedStatusBlock::kVersion;
    snapshot.length = sizeof(SharedStatusBlock);
    snapshot.reason = static_cast<uint32_t>(reason);
    snapshot.detailMask = detailMask;
    snapshot.updateTimestamp = mach_absolute_time();
    snapshot.sequence = ctx.statusSequence.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (ctx.controller) {
        const auto state = ctx.controller->StateMachine().CurrentState();
        snapshot.controllerState = static_cast<uint32_t>(state);
        auto stateName = std::string(ToString(state));
        std::strncpy(snapshot.controllerStateName,
                     stateName.c_str(),
                     sizeof(snapshot.controllerStateName) - 1);

        const auto& busMetrics = ctx.controller->Metrics().BusReset();
        snapshot.busResetCount = busMetrics.resetCount;
        snapshot.lastBusResetStart = busMetrics.lastResetStart;
        snapshot.lastBusResetCompletion = busMetrics.lastResetCompletion;

        if (auto topo = ctx.controller->LatestTopology()) {
            snapshot.busGeneration = topo->generation;
            snapshot.nodeCount = topo->nodeCount;
            if (topo->localNodeId.has_value()) {
                snapshot.localNodeID = static_cast<uint32_t>(*topo->localNodeId);
            }
            if (topo->rootNodeId.has_value()) {
                snapshot.rootNodeID = static_cast<uint32_t>(*topo->rootNodeId);
            }
            if (topo->irmNodeId.has_value()) {
                snapshot.irmNodeID = static_cast<uint32_t>(*topo->irmNodeId);
            }
            if (topo->irmNodeId.has_value() && topo->localNodeId.has_value() &&
                topo->irmNodeId == topo->localNodeId) {
                snapshot.flags |= SharedStatusBlock::kFlagIsIRM;
            }
        }
    }

    if (ctx.deps.asyncSubsystem) {
        const auto stats = ctx.deps.asyncSubsystem->GetWatchdogStats();
        snapshot.watchdogTickCount = stats.tickCount;
        snapshot.watchdogLastTickUsec = stats.lastTickUsec;
        snapshot.asyncTimeouts = static_cast<uint32_t>(stats.expiredTransactions);
        snapshot.asyncPending = 0; // Placeholder until OutstandingTable exposes count
    }

    snapshot.asyncLastCompletion = ctx.lastAsyncCompletionMach;
    snapshot.asyncTimeouts = ctx.asyncTimeoutCount;
    snapshot.watchdogTickCount = ctx.watchdogTickCount;
    snapshot.watchdogLastTickUsec = ctx.watchdogLastTickUsec;

    if (snapshot.localNodeID != 0xFFFFFFFFu) {
        snapshot.flags |= SharedStatusBlock::kFlagLinkActive;
    }

    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(ctx.statusBlock, &snapshot, sizeof(SharedStatusBlock));
    std::atomic_thread_fence(std::memory_order_release);

    if (ctx.statusListener) {
        ctx.statusListener->NotifyStatus(snapshot.sequence, snapshot.reason);
    }
}

void BindStatusListener(ServiceContext& ctx, ASFWDriverUserClient* client) {
    if (client) {
        ctx.statusListener = OSSharedPtr(client, OSRetain);
    } else {
        ctx.statusListener.reset();
    }
}

void UnbindStatusListener(ServiceContext& ctx, ASFWDriverUserClient* client) {
    if (ctx.statusListener && ctx.statusListener.get() == client) {
        ctx.statusListener.reset();
    }
}

kern_return_t CopyStatusSharedMemory(ServiceContext& ctx,
                                     uint64_t* options,
                                     IOMemoryDescriptor** memory) {
    if (!memory) {
        return kIOReturnBadArgument;
    }
    if (!ctx.statusMemory) {
        return kIOReturnNotReady;
    }

    auto descriptor = ctx.statusMemory.get();
    descriptor->retain();
    *memory = descriptor;
    if (options) {
        *options = kIOUserClientMemoryReadOnly;
    }
    return kIOReturnSuccess;
}

void EnsureDeps(ASFWDriver* driver, ServiceContext& ctx) {
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

    if (!d.avcDiscovery && d.deviceManager && d.asyncSubsystem) {
        d.avcDiscovery = std::make_shared<ASFW::Protocols::AVC::AVCDiscovery>(
            driver,
            *d.deviceManager,
            *d.asyncSubsystem
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

kern_return_t PrepareQueue(ASFWDriver& service, ServiceContext& ctx) {
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

kern_return_t PrepareInterrupts(ASFWDriver& service, IOService* provider, ServiceContext& ctx) {
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

kern_return_t PrepareWatchdog(ASFWDriver& service, ServiceContext& ctx) {
    if (!ctx.workQueue) {
        return kIOReturnNotReady;
    }

    IOTimerDispatchSource* timer = nullptr;
    auto kr = IOTimerDispatchSource::Create(ctx.workQueue.get(), &timer);
    if (kr != kIOReturnSuccess || !timer) {
        return kr != kIOReturnSuccess ? kr : kIOReturnNoResources;
    }
    ctx.watchdogTimer = OSSharedPtr(timer, OSNoRetain);

    OSAction* action = nullptr;
    kr = service.CreateActionAsyncWatchdogTimerFired(0, &action);
    if (kr != kIOReturnSuccess || !action) {
        ctx.watchdogTimer.reset();
        return kr != kIOReturnSuccess ? kr : kIOReturnError;
    }
    ctx.watchdogAction = OSSharedPtr(action, OSNoRetain);

    kr = ctx.watchdogTimer->SetHandler(ctx.watchdogAction.get());
    if (kr != kIOReturnSuccess) {
        ctx.watchdogAction.reset();
        ctx.watchdogTimer.reset();
        return kr;
    }

    kr = ctx.watchdogTimer->SetEnableWithCompletion(true, nullptr);
    if (kr != kIOReturnSuccess) {
        ctx.watchdogAction.reset();
        ctx.watchdogTimer.reset();
        return kr;
    }

    return kIOReturnSuccess;
}

void CleanupStartFailure(ServiceContext& ctx) {
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
    if (ctx.watchdogTimer) {
        ctx.watchdogTimer->Cancel(nullptr);
    }
    ctx.watchdogAction.reset();
    ctx.watchdogTimer.reset();
    ctx.workQueue.reset();
    ctx.statusListener.reset();
    ctx.statusBlock = nullptr;
    ctx.statusMap.reset();
    ctx.statusMemory.reset();
    ctx.statusSequence.store(0);
    ctx.lastAsyncCompletionMach = 0;
    ctx.asyncTimeoutCount = 0;
    ctx.watchdogTickCount = 0;
    ctx.watchdogLastTickUsec = 0;
}
} // namespace

bool ASFWDriver::init() {
    if (!super::init()) return false;
    if (!ivars) {
        ivars = IONewZero(ASFWDriver_IVars, 1);
        if (!ivars) return false;
    }
    if (!ivars->context) {
        ivars->context = new (std::nothrow) ServiceContext;
        if (!ivars->context) return false;
    }
    return true;
}

void ASFWDriver::free() {
    if (ivars) {
        if (ivars->context) {
            ivars->context->Reset();
            delete ivars->context;
            ivars->context = nullptr;
        }
        IODelete(ivars, ASFWDriver_IVars, 1);
        ivars = nullptr;
    }
    super::free();
}

kern_return_t IMPL(ASFWDriver, Start) {
    auto kr = Start(provider, SUPERDISPATCH);
    if (kr != kIOReturnSuccess) return kr;
    if (!ivars || !ivars->context) return kIOReturnNoMemory;
    auto& ctx = *ivars->context;
    ctx.stopping.store(false, std::memory_order_release);
    EnsureDeps(this, ctx);
    bool traceProperty = false;
    OSDictionary* serviceProperties = nullptr;
    if (CopyProperties(&serviceProperties) == kIOReturnSuccess && serviceProperties != nullptr) {
        if (auto property = serviceProperties->getObject("ASFWTraceDMACoherency")) {
            if (auto booleanProp = OSDynamicCast(OSBoolean, property)) {
                traceProperty = (booleanProp == kOSBooleanTrue);
            } else if (auto numberProp = OSDynamicCast(OSNumber, property)) {
                traceProperty = numberProp->unsigned32BitValue() != 0;
            } else if (auto stringProp = OSDynamicCast(OSString, property)) {
                traceProperty = stringProp->isEqualTo("1") ||
                                stringProp->isEqualTo("true") ||
                                stringProp->isEqualTo("TRUE");
            }
        }
        serviceProperties->release();
    }
    ASFW_LOG(Controller,
             "ASFWDriver::Start(): ASFWTraceDMACoherency property=%{public}s",
             traceProperty ? "true" : "false");
    auto statusKr = PrepareStatusBlock(ctx);
    if (statusKr != kIOReturnSuccess) {
        CleanupStartFailure(ctx);
        return statusKr;
    }
    kr = PrepareQueue(*this, ctx);
    if (kr != kIOReturnSuccess) { CleanupStartFailure(ctx); return kr; }
    kr = ctx.deps.hardware->Attach(this, provider);
    if (kr != kIOReturnSuccess) { CleanupStartFailure(ctx); return kr; }
    kr = PrepareInterrupts(*this, provider, ctx);
    if (kr != kIOReturnSuccess) { CleanupStartFailure(ctx); return kr; }

    // Initialize AsyncSubsystem (requires hardware, workQueue, and a completion action)
    if (ctx.deps.asyncSubsystem && ctx.deps.hardware && ctx.workQueue && ctx.interruptAction) {
        kr = ctx.deps.asyncSubsystem->Start(*ctx.deps.hardware, this, ctx.workQueue.get(), ctx.interruptAction.get());
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Controller, "AsyncSubsystem::Start() failed: 0x%08x", kr);
            CleanupStartFailure(ctx);
            return kr;
        }
        const bool traceActive = ASFW::Shared::DMAMemoryManager::IsTracingEnabled();
        ASFW_LOG(Controller,
                 "ASFWDriver::Start(): DMA coherency tracing %{public}s (requested=%{public}s)",
                 traceActive ? "ENABLED" : "disabled",
                 traceProperty ? "true" : "false");

        // CRITICAL: Re-run EnsureDeps to wire up PacketRouter handlers now that AsyncSubsystem is started
        // This ensures FCPResponseRouter registers its handler with the newly created PacketRouter
        EnsureDeps(this, ctx);
    }

    kr = PrepareWatchdog(*this, ctx);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Controller, "Failed to prepare async watchdog: 0x%08x", kr);
        CleanupStartFailure(ctx);
        return kr;
    }
    ScheduleAsyncWatchdog(kAsyncWatchdogPeriodUsec);

    ctx.controller = std::make_shared<ControllerCore>(ctx.config, ctx.deps);

    if (ctx.deps.speedPolicy) {
        if (!ctx.deps.romScanner) {
            OSSharedPtr<IODispatchQueue> discoveryQueue = nullptr;
            if (ctx.deps.scheduler) {
                discoveryQueue = ctx.deps.scheduler->Queue();
            }
            ctx.deps.romScanner = std::make_shared<ASFW::Discovery::ROMScanner>(
                ctx.controller->Bus(),
                *ctx.deps.speedPolicy,
                nullptr,
                discoveryQueue
            );
            ASFW_LOG(Controller, "✅ ROMScanner created");
        } else {
            ASFW_LOG(Controller, "Reusing existing ROMScanner instance");
        }

        if (ctx.deps.romScanner) {
            ctx.controller->AttachROMScanner(ctx.deps.romScanner);
        }
    }

    kr = ctx.controller->Start(provider);
    if (kr != kIOReturnSuccess) { CleanupStartFailure(ctx); return kr; }

    if (!ctx.deps.irmClient) {
        ctx.deps.irmClient = std::shared_ptr<ASFW::IRM::IRMClient>(
            new ASFW::IRM::IRMClient(ctx.controller->Bus()));
        ctx.controller->SetIRMClient(ctx.deps.irmClient);
        ASFW_LOG(Controller, "✅ IRMClient initialized");
    }
    
    if (!ctx.deps.cmpClient) {
        ctx.deps.cmpClient = std::shared_ptr<ASFW::CMP::CMPClient>(
            new ASFW::CMP::CMPClient(ctx.controller->Bus()));
        ctx.controller->SetCMPClient(ctx.deps.cmpClient);
        ASFW_LOG(Controller, "✅ CMPClient initialized");
    }

    ASFW::LogConfig::Shared().Initialize(this);

    PublishStatus(ctx, SharedStatusReason::Boot);
    
    const uint32_t initialMask = IntMaskBits::kMasterIntEnable | kBaseIntMask;
    ctx.deps.hardware->IntMaskSet(initialMask);
    
    RegisterService();
    ASFW_LOG(Controller, "ASFWDriver::Start() complete");
    
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWDriver, Stop) {
    if (ivars && ivars->context) {
        auto& ctx = *ivars->context;
        ctx.stopping.store(true, std::memory_order_release);
        ctx.statusListener.reset();
        PublishStatus(ctx, SharedStatusReason::Disconnect);
        if (ctx.deps.asyncSubsystem) {
            ctx.deps.asyncSubsystem->Stop();
        }
        if (ctx.controller) {
            ctx.controller->Stop();
        }
        if (ctx.deps.interrupts) {
            // Disable master interrupt before teardown
            ctx.deps.hardware->IntMaskClear(IntMaskBits::kMasterIntEnable | 0xFFFFFFFF);
            ctx.deps.interrupts->Disable();
        }
        if (ctx.deps.selfId && ctx.deps.hardware) ctx.deps.selfId->Disarm(*ctx.deps.hardware);
        if (ctx.deps.selfId) ctx.deps.selfId->ReleaseBuffers();
        if (ctx.deps.configRomStager && ctx.deps.hardware) ctx.deps.configRomStager->Teardown(*ctx.deps.hardware);
        if (ctx.deps.hardware) ctx.deps.hardware->Detach();
        if (ctx.watchdogTimer) {
            ctx.watchdogTimer->SetEnableWithCompletion(false, nullptr);
            ctx.watchdogTimer->Cancel(nullptr);
        }
    }
    return Stop(provider, SUPERDISPATCH);
}

kern_return_t ASFWDriver::CopyControllerStatus(OSDictionary** status) {
    if (!status) return kIOReturnBadArgument;
    *status = nullptr;
    auto dict = OSDictionary::withCapacity(4);
    if (!dict) return kIOReturnNoMemory;
    if (ivars && ivars->context && ivars->context->controller) {
        auto& controller = *ivars->context->controller;
        auto stateStr = std::string(ToString(controller.StateMachine().CurrentState()));
        if (auto s = OSSharedPtr<OSString>(OSString::withCString(stateStr.c_str()), OSNoRetain)) {
            dict->setObject("state", s.get());
        }
        auto& m = controller.Metrics().BusReset();
        if (auto n = OSSharedPtr<OSNumber>(OSNumber::withNumber(m.resetCount, 32), OSNoRetain)) {
            dict->setObject("busResetCount", n.get());
        }
        if (auto n = OSSharedPtr<OSNumber>(OSNumber::withNumber(m.lastResetStart, 64), OSNoRetain)) {
            dict->setObject("lastResetStart", n.get());
        }
        if (auto n = OSSharedPtr<OSNumber>(OSNumber::withNumber(m.lastResetCompletion, 64), OSNoRetain)) {
            dict->setObject("lastResetCompletion", n.get());
        }
        if (!m.lastFailureReason.has_value()) {
            dict->removeObject("lastResetFailure");
        } else if (auto s = OSSharedPtr<OSString>(OSString::withCString(m.lastFailureReason->c_str()), OSNoRetain)) {
            dict->setObject("lastResetFailure", s.get());
        }

        if (auto topo = controller.LatestTopology()) {
            if (auto n = OSSharedPtr<OSNumber>(OSNumber::withNumber(topo->generation, 32), OSNoRetain)) {
                dict->setObject("topologyGeneration", n.get());
            }
            if (auto n = OSSharedPtr<OSNumber>(OSNumber::withNumber(static_cast<uint64_t>(topo->nodes.size()), 32), OSNoRetain)) {
                dict->setObject("topologyNodeCount", n.get());
            }
        }
    }
    *status = dict;
    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::CopyControllerSnapshot(OSDictionary** status,
                                                 uint64_t* sequence,
                                                 uint64_t* timestamp) {
    if (status) {
        auto kr = CopyControllerStatus(status);
        if (kr != kIOReturnSuccess) {
            return kr;
        }
    }

    if (sequence) {
        *sequence = 0;
    }
    if (timestamp) {
        *timestamp = 0;
    }

    if (!ivars || !ivars->context || !ivars->context->statusBlock) {
        return kIOReturnSuccess;
    }

    const auto& block = *ivars->context->statusBlock;
    if (sequence) {
        *sequence = block.sequence;
    }
    if (timestamp) {
        *timestamp = block.updateTimestamp;
    }

    return kIOReturnSuccess;
}

void* ASFWDriver::GetControllerCore() const {
    if (!ivars || !ivars->context) return nullptr;
    return ivars->context->controller.get();
}

void* ASFWDriver::GetAsyncSubsystem() const {
    if (!ivars || !ivars->context) return nullptr;
    return ivars->context->deps.asyncSubsystem.get();
}

void* ASFWDriver::GetServiceContext() const {
    if (!ivars) return nullptr;
    return ivars->context;
}

kern_return_t IMPL(ASFWDriver, NewUserClient)
{
    if (type != 0) {
        return kIOReturnBadArgument;
    }

    if (!userClient) {
        return kIOReturnBadArgument;
    }

    ASFW_LOG(Controller, "NewUserClient request received (type=%u)", type);

    IOService* userClientService = nullptr;
    auto ret = Create(this, "ASFWDriverUserClientProperties", &userClientService);
    if (ret != kIOReturnSuccess || !userClientService) {
        ASFW_LOG(Controller, "NewUserClient Create failed: 0x%08x", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoResources;
    }

    auto client = OSDynamicCast(ASFWDriverUserClient, userClientService);
    if (!client) {
        ASFW_LOG(Controller, "NewUserClient cast failure");
        userClientService->release();
        return kIOReturnNoResources;
    }

    ret = client->Start(this);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(Controller, "NewUserClient Start failed: 0x%08x", ret);
        client->release();
        return ret;
    }

    *userClient = client;
    ASFW_LOG(Controller, "NewUserClient success (client=%p)", client);
    return kIOReturnSuccess;
}

void ASFWDriver::InterruptOccurred_Impl(ASFWDriver_InterruptOccurred_Args) {
    (void)action;
    (void)count;
    
    // DIAGNOSTIC: Log every interrupt invocation
    ASFW_LOG_V3(Controller, "InterruptOccurred called: time=%llu count=%llu", time, count);
    
    if (!ivars || !ivars->context) {
        ASFW_LOG(Controller, "InterruptOccurred: no ivars or context");
        return;
    }
    auto& ctx = *ivars->context;
    if (ctx.stopping.load(std::memory_order_acquire)) {
        return;
    }
    if (!ctx.controller || !ctx.deps.hardware) {
        ASFW_LOG(Controller, "InterruptOccurred: no controller or hardware");
        return;
    }
    auto snap = ctx.deps.hardware->CaptureInterruptSnapshot(time);
    ASFW_LOG_V2(Controller, "InterruptOccurred: captured snapshot intEvent=0x%08x", snap.intEvent);
    ctx.controller->HandleInterrupt(snap);
    
    // ===== ISOCHRONOUS RECEIVE INTERRUPT =====
    // Per OHCI §9.1: kIsochRx (bit 7) indicates one or more IR contexts have completed descriptors.
    // We read isoRecvEvent to determine which contexts, clear it, then dispatch processing.
    if ((snap.intEvent & IntEventBits::kIsochRx) && snap.isoRecvEvent != 0) {
        
        // DEBUG: Verify interrupt rate (sample every 100th)
        static uint32_t rxIrqCtr = 0;
        if ((++rxIrqCtr % 100) == 0) {
             ASFW_LOG(Controller, "[IRQ] IsoRx Fired! Count=%u Event=0x%08x IsoEvent=0x%08x", rxIrqCtr, snap.intEvent, snap.isoRecvEvent);
        }

        // Clear the per-context event bits to acknowledge
        ctx.deps.hardware->Write(Register32::kIsoRecvIntEventClear, snap.isoRecvEvent);
        
        // Context 0 is our single IR context for now
        if ((snap.isoRecvEvent & 0x01) && ctx.isochReceiveContext) {
            // Dispatch descriptor processing to workqueue (deferred from ISR)
            ctx.workQueue->DispatchAsync(^{
                if (ctx.isochReceiveContext) {
                    ctx.isochReceiveContext->Poll();
                }
            });
        }
    }

    // ===== ISOCHRONOUS TRANSMIT INTERRUPT =====
    // Per OHCI §9.2: kIsochTx (bit 6) indicates IT context completion.
    // Similar to IR, we read IsoXmitEvent, clear it, and process.
    if ((snap.intEvent & IntEventBits::kIsochTx) && snap.isoXmitEvent != 0) {
        
        // DEBUG: Sample interrupt rate
        static uint32_t txIrqCtr = 0;
        if ((++txIrqCtr % 100) == 0) {
            ASFW_LOG(Controller, "[IRQ] IsoTx Fired! Count=%u IsoTxEvent=0x%08x", txIrqCtr, snap.isoXmitEvent);
        }

        // Clear event bits to acknowledge
        ctx.deps.hardware->Write(Register32::kIsoXmitIntEventClear, snap.isoXmitEvent);
        
        // Context 0 is our single IT context
        if ((snap.isoXmitEvent & 0x01) && ctx.isochTransmitContext) {
            // Process IT directly in ISR context for lowest latency.
            // IT RefillRing is fast (atomic assemble + mem writes).
            // DispatchAsync adds latency which might cause underruns if buffers are small.
            ctx.isochTransmitContext->HandleInterrupt();
        }
    }

    if (snap.intEvent != 0) {
        const uint32_t asyncMask = IntEventBits::kReqTxComplete | IntEventBits::kRespTxComplete |
                                   IntEventBits::kARRQ | IntEventBits::kARRS |
                                   IntEventBits::kRQPkt | IntEventBits::kRSPkt;
        if (snap.intEvent & asyncMask) {
            ctx.lastAsyncCompletionMach = mach_absolute_time();
        }

        SharedStatusReason reason = SharedStatusReason::Interrupt;
        if (snap.intEvent & IntEventBits::kBusReset) {
            reason = SharedStatusReason::BusReset;
        } else if (snap.intEvent & asyncMask) {
            reason = SharedStatusReason::AsyncActivity;
        } else if (snap.intEvent & IntEventBits::kUnrecoverableError) {
            reason = SharedStatusReason::Interrupt;
        }

        PublishStatus(ctx, reason, snap.intEvent);
    }
}

void ASFWDriver::ScheduleAsyncWatchdog(uint64_t delayUsec) {
    if (!ivars || !ivars->context) {
        return;
    }
    auto& ctx = *ivars->context;
    if (ctx.stopping.load(std::memory_order_acquire)) {
        return;
    }
    if (!ctx.watchdogTimer) {
        return;
    }

    const uint64_t now = mach_absolute_time();
    const uint64_t delta = MicrosecondsToMachTicks(delayUsec);
    (void)ctx.watchdogTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime, now + delta, 0);
}

void ASFWDriver::AsyncWatchdogTimerFired_Impl(ASFWDriver_AsyncWatchdogTimerFired_Args) {
    (void)action;
    (void)time;

    if (ivars && ivars->context) {
        auto& ctx = *ivars->context;
        if (ctx.stopping.load(std::memory_order_acquire)) {
            return;
        }
        if (ctx.deps.asyncSubsystem) {
            ctx.deps.asyncSubsystem->OnTimeoutTick();
            const auto stats = ctx.deps.asyncSubsystem->GetWatchdogStats();
            ctx.asyncTimeoutCount = static_cast<uint32_t>(stats.expiredTransactions);
            ctx.watchdogTickCount = stats.tickCount;
            ctx.watchdogLastTickUsec = stats.lastTickUsec;
        }
        
        // Phase 1: Poll Isoch Context (Simple Polling)
        if (ctx.isochReceiveContext) {
            // Only poll if running
            if (ctx.isochReceiveContext->GetState() == ASFW::Isoch::IRPolicy::State::Running) {
                ctx.isochReceiveContext->Poll();
            }
            // Periodic Logging (every ~1s = 500 ticks @ 2ms)
            if (++ctx.isochLogDivider >= 500) {
                ctx.isochLogDivider = 0;
                if (ctx.isochReceiveContext->GetState() == ASFW::Isoch::IRPolicy::State::Running) {
                     // Log high-level stats
                     ctx.isochReceiveContext->GetStreamProcessor().LogStatistics();
                     // EXPERT DEBUG: Log raw hardware state (registers + descriptor)
                     ctx.isochReceiveContext->LogHardwareState();
                }
            }
        }
        
        // Phase 1.5/2: Poll Isoch Transmit Context
        if (ctx.isochTransmitContext) {
            // Poll to refill descriptors as hardware consumes them
            // The 1ms watchdog provides reliable refill timing
            if (ctx.isochTransmitContext->GetState() == ASFW::Isoch::ITState::Running) {
                ctx.isochTransmitContext->Poll();
            }

            // Periodic logging every ~1s (1000 ticks @ 1ms)
            if (++ctx.itLogDivider >= 1000) {
                ctx.itLogDivider = 0;
                if (ctx.isochTransmitContext->GetState() == ASFW::Isoch::ITState::Running) {
                    ctx.isochTransmitContext->LogStatistics();
                }
            }
        }
        
        PublishStatus(ctx, SharedStatusReason::Watchdog);
    }

    ScheduleAsyncWatchdog(kAsyncWatchdogPeriodUsec);
}

void ASFWDriver::RegisterStatusListener(OSObject* client) {
    auto* clientObj = OSDynamicCast(ASFWDriverUserClient, client);
    if (!clientObj || !ivars || !ivars->context) {
        return;
    }

    auto& ctx = *ivars->context;
    BindStatusListener(ctx, clientObj);
    PublishStatus(ctx, SharedStatusReason::Manual);
}

void ASFWDriver::UnregisterStatusListener(OSObject* client) {
    auto* clientObj = OSDynamicCast(ASFWDriverUserClient, client);
    if (!clientObj || !ivars || !ivars->context) {
        return;
    }

    UnbindStatusListener(*ivars->context, clientObj);
}

kern_return_t ASFWDriver::CopySharedStatusMemory(uint64_t* options,
                                                 IOMemoryDescriptor** memory) {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }

    return CopyStatusSharedMemory(*ivars->context, options, memory);
}

// Runtime logging configuration methods
kern_return_t ASFWDriver::SetAsyncVerbosity(uint32_t level) {
    ASFW_LOG_INFO(Controller, "UserClient: Setting async verbosity to %u", level);
    ASFW::LogConfig::Shared().SetAsyncVerbosity(static_cast<uint8_t>(level));
    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::SetHexDumps(uint32_t enabled) {
    ASFW_LOG_INFO(Controller, "UserClient: Setting hex dumps to %{public}s", enabled ? "enabled" : "disabled");
    ASFW::LogConfig::Shared().SetHexDumps(enabled != 0);
    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::GetLogConfig(uint32_t* asyncVerbosity,
                                      uint32_t* hexDumpsEnabled) {
    if (!asyncVerbosity || !hexDumpsEnabled) {
        return kIOReturnBadArgument;
    }
    *asyncVerbosity = ASFW::LogConfig::Shared().GetAsyncVerbosity();
    *hexDumpsEnabled = ASFW::LogConfig::Shared().IsHexDumpsEnabled() ? 1 : 0;
    ASFW_LOG_INFO(Controller, "UserClient: Reading log configuration (Async=%u, HexDumps=%d)",
                  *asyncVerbosity, *hexDumpsEnabled);
    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::StartIsochReceive(uint8_t channel) {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }
    if (!ivars->context->isochReceiveContext) {
        if (!ivars->context->deps.asyncSubsystem) {
             ASFW_LOG(Controller, "[Isoch] ❌ StartIsochReceive: Subsystems not ready");
             return kIOReturnNotReady;
        }
        
        ASFW::Isoch::Memory::IsochMemoryConfig config;
        config.numDescriptors = ASFW::Isoch::IsochReceiveContext::kNumDescriptors;
        config.packetSizeBytes = ASFW::Isoch::IsochReceiveContext::kMaxPacketSize;
        config.descriptorAlignment = 16;
        config.payloadPageAlignment = 16384;

        auto isochMem = ASFW::Isoch::Memory::IsochDMAMemoryManager::Create(config);
        if (!isochMem) {
             ASFW_LOG(Controller, "[Isoch] ❌ StartIsochReceive: Failed to create Memory Manager");
             return kIOReturnNoMemory;
        }
        
        if (!isochMem->Initialize(*ivars->context->deps.hardware)) {
             ASFW_LOG(Controller, "[Isoch] ❌ StartIsochReceive: Failed to initialize DMA slabs");
             return kIOReturnNoMemory;
        }

        ivars->context->isochReceiveContext = ASFW::Isoch::IsochReceiveContext::Create(
            ivars->context->deps.hardware.get(), 
            isochMem
        );
        
        if (!ivars->context->isochReceiveContext) {
             ASFW_LOG(Controller, "[Isoch] ❌ StartIsochReceive: Context creation failed");
             return kIOReturnNoMemory;
        }
        ASFW_LOG(Controller, "[Isoch] ✅ provisioned Isoch Context with Dedicated Memory");
    }
    
    if (!ivars->context->isochReceiveContext) {
        ASFW_LOG(Controller, "[Isoch] ❌ StartIsochReceive: Context not initialized");
        return kIOReturnNotReady;
    }
    
    auto& ctx = *ivars->context;
    
    // Configure for Channel N, Context 0 (first IR context)
    auto result = ctx.isochReceiveContext->Configure(channel, 0); 
    if (result != kIOReturnSuccess) {
         ASFW_LOG(Controller, "[Isoch] ❌ Failed to Configure IR Context: 0x%x", result);
         return result;
    }
    
    result = ctx.isochReceiveContext->Start();
    if (result != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[Isoch] ❌ Failed to Start IR Context: 0x%x", result);
        return result;
    }
    
    ASFW_LOG(Controller, "[Isoch] ✅ Started IR Context 0 for Channel %u!", channel);
    
    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::StopIsochReceive() {
    if (!ivars || !ivars->context || !ivars->context->isochReceiveContext) {
        return kIOReturnNotReady;
    }
    ivars->context->isochReceiveContext->Stop();
    ASFW_LOG(Controller, "[Isoch] Stopped IR Context 0");
    return kIOReturnSuccess;
}

void* ASFWDriver::GetIsochReceiveContext() const {
    if (!ivars || !ivars->context) {
        return nullptr;
    }
    return ivars->context->isochReceiveContext.get();
}

// =============================================================================
// MARK: - Isochronous Transmit
// =============================================================================

kern_return_t ASFWDriver::StartIsochTransmit(uint8_t channel) {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }
    
    if (!ivars->context->isochTransmitContext) {
        if (!ivars->context->deps.asyncSubsystem) {
             ASFW_LOG(Controller, "[Isoch] ❌ StartIsochTransmit: Subsystems not ready");
             return kIOReturnNotReady;
        }
        
        ASFW::Isoch::Memory::IsochMemoryConfig config;
        config.numDescriptors = ASFW::Isoch::IsochTransmitContext::kNumDescriptors;
        config.packetSizeBytes = ASFW::Isoch::IsochTransmitContext::kMaxPacketSize;
        config.descriptorAlignment = 16;
        config.payloadPageAlignment = 16384;

        auto isochMem = ASFW::Isoch::Memory::IsochDMAMemoryManager::Create(config);
        if (!isochMem) {
             ASFW_LOG(Controller, "[Isoch] ❌ StartIsochTransmit: Failed to create Memory Manager");
             return kIOReturnNoMemory;
        }
        
        if (!isochMem->Initialize(*ivars->context->deps.hardware)) {
             ASFW_LOG(Controller, "[Isoch] ❌ StartIsochTransmit: Failed to initialize DMA slabs");
             return kIOReturnNoMemory;
        }

        ivars->context->isochTransmitContext = ASFW::Isoch::IsochTransmitContext::Create(
            ivars->context->deps.hardware.get(), 
            isochMem
        );
        
        if (!ivars->context->isochTransmitContext) {
             ASFW_LOG(Controller, "[Isoch] ❌ StartIsochTransmit: Context creation failed");
             return kIOReturnNoMemory;
        }
        ASFW_LOG(Controller, "[Isoch] ✅ provisioned IT Context with Dedicated Memory");
    }
    
    if (!ivars->context->isochTransmitContext) {
        ASFW_LOG(Controller, "[Isoch] ❌ StartIsochTransmit: Context not initialized");
        return kIOReturnNotReady;
    }
    
    auto& ctx = *ivars->context;
    
    uint8_t localNodeId = 0;
    
    auto result = ctx.isochTransmitContext->Configure(channel, localNodeId);
    if (result != kIOReturnSuccess) {
         ASFW_LOG(Controller, "[Isoch] ❌ Failed to Configure IT Context: 0x%x", result);
         return result;
    }

    if (ctx.deps.avcDiscovery) {
        ASFWAudioNub* audioNub = ctx.deps.avcDiscovery->GetFirstAudioNub();
        if (audioNub) {
            void* txQueueBase = audioNub->GetTxQueueLocalMapping();
            uint64_t txQueueBytes = audioNub->GetTxQueueBytes();

            if (txQueueBase && txQueueBytes > 0) {
                ctx.isochTransmitContext->SetSharedTxQueue(txQueueBase, txQueueBytes);
                ASFW_LOG(Controller, "[Isoch] Wired shared TX queue to IT context (bytes=%llu)",
                         txQueueBytes);
            }
        }
    }

    uint32_t fillLevel = 0;
    const uint32_t targetFill = 512;
    const int maxWaitMs = 100;

    for (int waitMs = 0; waitMs < maxWaitMs; waitMs += 5) {
        fillLevel = ctx.isochTransmitContext->SharedTxFillLevelFrames();
        if (fillLevel >= targetFill) {
            break;
        }
        IOSleep(5);
    }

    result = ctx.isochTransmitContext->Start();
    if (result != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[Isoch] Failed to Start IT Context: 0x%x", result);
        return result;
    }

    ASFW_LOG(Controller, "[Isoch] ✅ Started IT Context for Channel %u!", channel);

    if (ctx.deps.deviceManager && ctx.deps.cmpClient) {
        auto devices = ctx.deps.deviceManager->GetReadyDevices();
        if (!devices.empty()) {
            auto target = devices.front();
            uint16_t nodeId = target->GetNodeID();
            ASFW::IRM::Generation gen = target->GetGeneration();
            
            ctx.deps.cmpClient->SetDeviceNode(static_cast<uint8_t>(nodeId & 0x3F), gen);
            ctx.deps.cmpClient->ConnectIPCR(0, channel, [](ASFW::CMP::CMPStatus status) {
                if (status == ASFW::CMP::CMPStatus::Success) {
                    ASFW_LOG(Controller, "[Isoch] ✅ CMP ConnectIPCR Success!");
                } else {
                    ASFW_LOG(Controller, "[Isoch] ❌ CMP ConnectIPCR Failed: %d", static_cast<int>(status));
                }
            });
        }
    }

    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::StopIsochTransmit() {
    if (!ivars || !ivars->context || !ivars->context->isochTransmitContext) {
        return kIOReturnNotReady;
    }

    ivars->context->isochTransmitContext->Stop();
    ASFW_LOG(Controller, "[Isoch] Stopped IT Context");

    if (ivars->context->deps.cmpClient) {
         ivars->context->deps.cmpClient->DisconnectIPCR(0, [](ASFW::CMP::CMPStatus status){
              if (status == ASFW::CMP::CMPStatus::Success) {
                    ASFW_LOG(Controller, "[Isoch] ✅ CMP DisconnectIPCR Success!");
                } else {
                    ASFW_LOG(Controller, "[Isoch] ❌ CMP DisconnectIPCR Failed: %d", static_cast<int>(status));
                }
         });
    }

    return kIOReturnSuccess;
}

void* ASFWDriver::GetIsochTransmitContext() const {
    if (!ivars || !ivars->context) {
        return nullptr;
    }
    return ivars->context->isochTransmitContext.get();
}
