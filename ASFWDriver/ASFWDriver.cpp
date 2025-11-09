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

#include "Core/ControllerTypes.hpp"
#include "Core/ControllerCore.hpp"
#include "Core/ControllerConfig.hpp"
#include "Core/HardwareInterface.hpp"
#include "Core/InterruptManager.hpp"
#include "Core/Scheduler.hpp"
#include "Core/BusResetCoordinator.hpp"
#include "Core/SelfIDCapture.hpp"
#include "Core/TopologyManager.hpp"
#include "Core/MetricsSink.hpp"
#include "Core/ControllerStateMachine.hpp"
#include "Core/ConfigROMBuilder.hpp"
#include "Core/ConfigROMStager.hpp"
#include "Logging/Logging.hpp"
#include "Core/OHCIConstants.hpp"
#include "Core/RegisterMap.hpp"
#include "Async/AsyncSubsystem.hpp"
#include "Async/Core/DMAMemoryManager.hpp"
#include "Discovery/SpeedPolicy.hpp"
#include "Discovery/ConfigROMStore.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Discovery/ROMScanner.hpp"
#include "Discovery/ROMReader.hpp"

using namespace ASFW::Driver;

class ASFWDriverUserClient;

namespace {
constexpr uint64_t kAsyncWatchdogPeriodUsec = 2000; // 2 ms tick

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
    ASFWDriverUserClient* statusListener{nullptr};
    uint64_t lastAsyncCompletionMach{0};
    uint32_t asyncTimeoutCount{0};
    uint64_t watchdogTickCount{0};
    uint64_t watchdogLastTickUsec{0};
    void Reset() {
        controller.reset();
        deps.hardware.reset();
        deps.busReset.reset();
        deps.selfId.reset();
        deps.scheduler.reset();
        deps.metrics.reset();
        deps.stateMachine.reset();
        deps.configRom.reset();
        deps.configRomStager.reset();
        deps.interrupts.reset();
        deps.asyncSubsystem.reset();  // Stop and cleanup asyncSubsystem
        statusListener = nullptr;
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
    }
};

namespace {
constexpr uint64_t kSharedStatusMemoryType = 0;

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
    ctx.statusListener = client;
}

void UnbindStatusListener(ServiceContext& ctx, ASFWDriverUserClient* client) {
    if (ctx.statusListener == client) {
        ctx.statusListener = nullptr;
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

void EnsureDeps(ServiceContext& ctx) {
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

    // Phase 2A: Enable AsyncSubsystem creation (memory allocation only)
    // DMA context arming still disabled - that's Phase 2B/2D
    if (!d.asyncSubsystem) {
        d.asyncSubsystem = std::make_shared<ASFW::Async::AsyncSubsystem>();
    }

    // Discovery subsystem (requires AsyncSubsystem for ROMReader)
    if (!d.speedPolicy) {
        d.speedPolicy = std::make_shared<ASFW::Discovery::SpeedPolicy>();
    }
    if (!d.romStore) {
        d.romStore = std::make_shared<ASFW::Discovery::ConfigROMStore>();
    }
    if (!d.deviceRegistry) {
        d.deviceRegistry = std::make_shared<ASFW::Discovery::DeviceRegistry>();
    }
    if (!d.romScanner) {
        // ROMScanner manages its own parameters internally (SSOT)
        // ROM size determined dynamically from BIB crc_length field
        d.romScanner = std::make_shared<ASFW::Discovery::ROMScanner>(
            *d.asyncSubsystem,
            *d.speedPolicy
        );
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
    ctx.statusListener = nullptr;
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
    EnsureDeps(ctx);
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
        const bool traceActive = ASFW::Async::DMAMemoryManager::IsTracingEnabled();
        ASFW_LOG(Controller,
                 "ASFWDriver::Start(): DMA coherency tracing %{public}s (requested=%{public}s)",
                 traceActive ? "ENABLED" : "disabled",
                 traceProperty ? "true" : "false");
    }

    kr = PrepareWatchdog(*this, ctx);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Controller, "Failed to prepare async watchdog: 0x%08x", kr);
        CleanupStartFailure(ctx);
        return kr;
    }
    ScheduleAsyncWatchdog(kAsyncWatchdogPeriodUsec);

    ctx.controller = std::make_shared<ControllerCore>(ctx.config, ctx.deps);
    kr = ctx.controller->Start(provider);
    if (kr != kIOReturnSuccess) { CleanupStartFailure(ctx); return kr; }
    
    PublishStatus(ctx, SharedStatusReason::Boot);
    
    // CRITICAL: Register service to enable IOKit matching and UserClient connections
    RegisterService();
    ASFW_LOG(Controller, "ASFWDriver::Start() complete - service registered");
    
    return kIOReturnSuccess;
}

kern_return_t IMPL(ASFWDriver, Stop) {
    if (ivars && ivars->context) {
        auto& ctx = *ivars->context;
        PublishStatus(ctx, SharedStatusReason::Disconnect);
        if (ctx.controller) {
            ctx.controller->Stop();
            ctx.controller.reset();
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
    }
    return super::Stop(provider);
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
    ASFW_LOG(Controller, "InterruptOccurred called: time=%llu count=%llu", time, count);
    
    if (!ivars || !ivars->context) {
        ASFW_LOG(Controller, "InterruptOccurred: no ivars or context");
        return;
    }
    auto& ctx = *ivars->context;
    if (!ctx.controller || !ctx.deps.hardware) {
        ASFW_LOG(Controller, "InterruptOccurred: no controller or hardware");
        return;
    }
    auto snap = ctx.deps.hardware->CaptureInterruptSnapshot(time);
    ASFW_LOG(Controller, "InterruptOccurred: captured snapshot intEvent=0x%08x", snap.intEvent);
    ctx.controller->HandleInterrupt(snap);

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
        if (ctx.deps.asyncSubsystem) {
            ctx.deps.asyncSubsystem->OnTimeoutTick();
            const auto stats = ctx.deps.asyncSubsystem->GetWatchdogStats();
            ctx.asyncTimeoutCount = static_cast<uint32_t>(stats.expiredTransactions);
            ctx.watchdogTickCount = stats.tickCount;
            ctx.watchdogLastTickUsec = stats.lastTickUsec;
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
