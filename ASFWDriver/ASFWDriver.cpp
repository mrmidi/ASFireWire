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

#include "Logging/Logging.hpp"
#include "Logging/LogConfig.hpp"
#include "Hardware/OHCIConstants.hpp"
#include "Hardware/RegisterMap.hpp"
#include "Hardware/InterruptManager.hpp"
#include "Scheduling/Scheduler.hpp"
#include "Shared/Memory/DMAMemoryManager.hpp"
#include "ConfigROM/ROMScanner.hpp"
#include "ConfigROM/ROMReader.hpp"
#include "ConfigROM/ConfigROMStager.hpp"
#include "IRM/IRMClient.hpp"
#include "Protocols/AVC/CMP/CMPClient.hpp"
#include "Async/AsyncSubsystem.hpp"
#include "Async/Interfaces/IFireWireBus.hpp"
#include "Async/DMAMemoryImpl.hpp"
#include "Bus/SelfIDCapture.hpp"
#include "Controller/ControllerStateMachine.hpp"
#include "Diagnostics/MetricsSink.hpp"
#include "Discovery/FWDevice.hpp"
#include "Service/DriverContext.hpp"
#include "Protocols/AVC/AVCDiscovery.hpp"
#include "Isoch/IsochReceiveContext.hpp"
#include "Isoch/Transmit/IsochTransmitContext.hpp"
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>

using namespace ASFW::Driver;

class ASFWDriverUserClient;

namespace {
constexpr uint64_t kAsyncWatchdogPeriodUsec = 1000; // 1 ms tick (hybrid: interrupt + timer backup)
} // namespace

bool ASFWDriver::init() {
    if (!super::init()) return false;
    if (!ivars) {
        ivars = IONewZero(ASFWDriver_IVars, 1);
        if (!ivars) return false;
    }
    if (!ivars->context) {
        ivars->context = IONew(ServiceContext, 1);
        if (!ivars->context) return false;
    }
    return true;
}

void ASFWDriver::free() {
    if (ivars) {
        if (ivars->context) {
            ivars->context->Reset();
            IOSafeDeleteNULL(ivars->context, ServiceContext, 1);
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
    DriverWiring::EnsureDeps(this, ctx);
    bool traceProperty = false;
    if (OSDictionary* serviceProperties = nullptr;
        CopyProperties(&serviceProperties) == kIOReturnSuccess && serviceProperties != nullptr) {
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
    if (auto statusKr = ctx.statusPublisher.Prepare(); statusKr != kIOReturnSuccess) {
        DriverWiring::CleanupStartFailure(ctx);
        return statusKr;
    }
    kr = DriverWiring::PrepareQueue(*this, ctx);
    if (kr != kIOReturnSuccess) { DriverWiring::CleanupStartFailure(ctx); return kr; }
    kr = ctx.deps.hardware->Attach(this, provider);
    if (kr != kIOReturnSuccess) { DriverWiring::CleanupStartFailure(ctx); return kr; }
    kr = DriverWiring::PrepareInterrupts(*this, provider, ctx);
    if (kr != kIOReturnSuccess) { DriverWiring::CleanupStartFailure(ctx); return kr; }

    // Initialize AsyncSubsystem (requires hardware, workQueue, and a completion action)
    if (ctx.deps.asyncSubsystem && ctx.deps.hardware && ctx.workQueue && ctx.interruptAction) {
        kr = ctx.deps.asyncSubsystem->Start(*ctx.deps.hardware, this, ctx.workQueue.get(), ctx.interruptAction.get());
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Controller, "AsyncSubsystem::Start() failed: 0x%08x", kr);
            DriverWiring::CleanupStartFailure(ctx);
            return kr;
        }
        const bool traceActive = ASFW::Shared::DMAMemoryManager::IsTracingEnabled();
        ASFW_LOG(Controller,
                 "ASFWDriver::Start(): DMA coherency tracing %{public}s (requested=%{public}s)",
                 traceActive ? "ENABLED" : "disabled",
                 traceProperty ? "true" : "false");

        // CRITICAL: Re-run EnsureDeps to wire up PacketRouter handlers now that AsyncSubsystem is started
        // This ensures FCPResponseRouter registers its handler with the newly created PacketRouter
        DriverWiring::EnsureDeps(this, ctx);
    }

    kr = DriverWiring::PrepareWatchdog(*this, ctx);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Controller, "Failed to prepare async watchdog: 0x%08x", kr);
        DriverWiring::CleanupStartFailure(ctx);
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
    if (kr != kIOReturnSuccess) { DriverWiring::CleanupStartFailure(ctx); return kr; }

    if (!ctx.deps.irmClient) {
        ctx.deps.irmClient = std::make_shared<ASFW::IRM::IRMClient>(ctx.controller->Bus());
        ctx.controller->SetIRMClient(ctx.deps.irmClient);
        ASFW_LOG(Controller, "✅ IRMClient initialized");
    }
    
    if (!ctx.deps.cmpClient) {
        ctx.deps.cmpClient = std::make_shared<ASFW::CMP::CMPClient>(ctx.controller->Bus());
        ctx.controller->SetCMPClient(ctx.deps.cmpClient);
        ASFW_LOG(Controller, "✅ CMPClient initialized");
    }

    ASFW::LogConfig::Shared().Initialize(this);

    ctx.statusPublisher.Publish(ctx.controller.get(),
                                ctx.deps.asyncSubsystem.get(),
                                SharedStatusReason::Boot);
    
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
        ctx.statusPublisher.BindListener(nullptr);
        ctx.statusPublisher.Publish(ctx.controller.get(),
                                    ctx.deps.asyncSubsystem.get(),
                                    SharedStatusReason::Disconnect);
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
        ctx.watchdog.Stop();
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

    if (!ivars || !ivars->context || !ivars->context->statusPublisher.StatusBlock()) {
        return kIOReturnSuccess;
    }

    const auto& block = *ivars->context->statusPublisher.StatusBlock();
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
    ctx.interruptDispatcher.HandleSnapshot(snap,
                                           *ctx.controller,
                                           *ctx.deps.hardware,
                                           *ctx.workQueue,
                                           ctx.isoch,
                                           ctx.statusPublisher,
                                           ctx.deps.asyncSubsystem.get());
}

void ASFWDriver::ScheduleAsyncWatchdog(uint64_t delayUsec) {
    if (!ivars || !ivars->context) {
        return;
    }
    auto& ctx = *ivars->context;
    if (ctx.stopping.load(std::memory_order_acquire)) {
        return;
    }
    ctx.watchdog.Schedule(delayUsec);
}

void ASFWDriver::AsyncWatchdogTimerFired_Impl(ASFWDriver_AsyncWatchdogTimerFired_Args) {
    (void)action;
    (void)time;

    if (ivars && ivars->context) {
        auto& ctx = *ivars->context;
        if (ctx.stopping.load(std::memory_order_acquire)) {
            return;
        }
        ctx.watchdog.HandleTick(ctx.controller.get(),
                                ctx.deps.asyncSubsystem.get(),
                                ctx.isoch.ReceiveContext(),
                                ctx.isoch.TransmitContext(),
                                ctx.statusPublisher);
    }

    ScheduleAsyncWatchdog(kAsyncWatchdogPeriodUsec);
}

void ASFWDriver::RegisterStatusListener(const OSObject* client) {
    auto* clientObj = OSDynamicCast(ASFWDriverUserClient, const_cast<OSObject*>(client));
    if (!clientObj || !ivars || !ivars->context) {
        return;
    }

    auto& ctx = *ivars->context;
    ctx.statusPublisher.BindListener(clientObj);
    ctx.statusPublisher.Publish(ctx.controller.get(),
                                ctx.deps.asyncSubsystem.get(),
                                SharedStatusReason::Manual);
}

void ASFWDriver::UnregisterStatusListener(const OSObject* client) {
    auto* clientObj = OSDynamicCast(ASFWDriverUserClient, const_cast<OSObject*>(client));
    if (!clientObj || !ivars || !ivars->context) {
        return;
    }

    ivars->context->statusPublisher.UnbindListener(clientObj);
}

kern_return_t ASFWDriver::CopySharedStatusMemory(uint64_t* options,
                                                 IOMemoryDescriptor** memory) const {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }

    return ivars->context->statusPublisher.CopySharedMemory(options, memory);
}

// Runtime logging configuration methods
kern_return_t ASFWDriver::SetAsyncVerbosity(uint32_t level) const {
    ASFW_LOG_INFO(Controller, "UserClient: Setting async verbosity to %u", level);
    ASFW::LogConfig::Shared().SetAsyncVerbosity(static_cast<uint8_t>(level));
    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::SetIsochVerbosity(uint32_t level) const {
    ASFW_LOG_INFO(Controller, "UserClient: Setting isoch verbosity to %u", level);
    ASFW::LogConfig::Shared().SetIsochVerbosity(static_cast<uint8_t>(level));
    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::SetHexDumps(uint32_t enabled) const {
    ASFW_LOG_INFO(Controller, "UserClient: Setting hex dumps to %{public}s", enabled ? "enabled" : "disabled");
    ASFW::LogConfig::Shared().SetHexDumps(enabled != 0);
    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::SetIsochTxVerifier(uint32_t enabled) const {
    ASFW_LOG_INFO(Controller, "UserClient: Setting isoch TX verifier to %{public}s",
                  enabled ? "enabled" : "disabled");
    ASFW::LogConfig::Shared().SetIsochTxVerifierEnabled(enabled != 0);
    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::GetLogConfig(uint32_t* asyncVerbosity,
                                      uint32_t* hexDumpsEnabled,
                                      uint32_t* isochVerbosity) const {
    if (!asyncVerbosity || !hexDumpsEnabled || !isochVerbosity) {
        return kIOReturnBadArgument;
    }
    *asyncVerbosity = ASFW::LogConfig::Shared().GetAsyncVerbosity();
    *hexDumpsEnabled = ASFW::LogConfig::Shared().IsHexDumpsEnabled() ? 1 : 0;
    *isochVerbosity = ASFW::LogConfig::Shared().GetIsochVerbosity();
    ASFW_LOG_INFO(Controller, "UserClient: Reading log configuration (Async=%u, Isoch=%u, HexDumps=%d)",
                  *asyncVerbosity, *isochVerbosity, *hexDumpsEnabled);
    return kIOReturnSuccess;
}

kern_return_t ASFWDriver::StartIsochReceive(uint8_t channel) {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }
    auto& ctx = *ivars->context;
    if (!ctx.deps.asyncSubsystem || !ctx.deps.hardware) {
        ASFW_LOG(Controller, "[Isoch] ❌ StartIsochReceive: Subsystems not ready");
        return kIOReturnNotReady;
    }

    return ctx.isoch.StartReceive(channel,
                                  *ctx.deps.hardware,
                                  ctx.deps.deviceManager.get(),
                                  ctx.deps.cmpClient.get(),
                                  ctx.deps.avcDiscovery.get());
}

kern_return_t ASFWDriver::StopIsochReceive() {
    if (!ivars || !ivars->context || !ivars->context->isoch.ReceiveContext()) {
        return kIOReturnNotReady;
    }
    return ivars->context->isoch.StopReceive(ivars->context->deps.cmpClient.get());
}

void* ASFWDriver::GetIsochReceiveContext() const {
    if (!ivars || !ivars->context) {
        return nullptr;
    }
    return ivars->context->isoch.ReceiveContext();
}

// =============================================================================
// MARK: - Isochronous Transmit
// =============================================================================

kern_return_t ASFWDriver::StartIsochTransmit(uint8_t channel) {
    if (!ivars || !ivars->context) {
        return kIOReturnNotReady;
    }
    auto& ctx = *ivars->context;
    if (!ctx.deps.asyncSubsystem || !ctx.deps.hardware) {
        ASFW_LOG(Controller, "[Isoch] ❌ StartIsochTransmit: Subsystems not ready");
        return kIOReturnNotReady;
    }

    return ctx.isoch.StartTransmit(channel,
                                   *ctx.deps.hardware,
                                   ctx.deps.deviceManager.get(),
                                   ctx.deps.cmpClient.get(),
                                   ctx.deps.avcDiscovery.get());
}

kern_return_t ASFWDriver::StopIsochTransmit() {
    if (!ivars || !ivars->context || !ivars->context->isoch.TransmitContext()) {
        return kIOReturnNotReady;
    }
    return ivars->context->isoch.StopTransmit(ivars->context->deps.cmpClient.get());
}

void* ASFWDriver::GetIsochTransmitContext() const {
    if (!ivars || !ivars->context) {
        return nullptr;
    }
    return ivars->context->isoch.TransmitContext();
}
