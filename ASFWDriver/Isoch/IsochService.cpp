#include "IsochService.hpp"

#include <DriverKit/IOLib.h>
#include <atomic>

#include "IsochReceiveContext.hpp"
#include "Transmit/IsochTransmitContext.hpp"
#include "Memory/IsochDMAMemoryManager.hpp"
#include "Config/AudioTxProfiles.hpp"
#include "Encoding/TimingUtils.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Logging/Logging.hpp"

namespace ASFW::Driver {

kern_return_t IsochService::StartReceive(uint8_t channel,
                                         HardwareInterface& hardware,
                                         IOBufferMemoryDescriptor* rxQueueMemory,
                                         uint64_t rxQueueBytes) {
    if (isochReceiveContext_ &&
        isochReceiveContext_->GetState() == ASFW::Isoch::IRPolicy::State::Running) {
        ASFW_LOG(Controller, "[Isoch] IR already running; StartReceive is idempotent");
        if (rxQueueMemory) {
            rxQueueMemory->release();
        }
        return kIOReturnSuccess;
    }

    rxQueue_.Reset();

    void* rxQueueBase = nullptr;
    if (rxQueueMemory && rxQueueBytes > 0) {
        rxQueue_.memory.reset(rxQueueMemory, OSNoRetain);
        rxQueue_.bytes = rxQueueBytes;

        IOMemoryMap* mapRaw = nullptr;
        const kern_return_t mappingStatus = rxQueueMemory->CreateMapping(
            kIOMemoryMapCacheModeDefault,
            0,
            0,
            0,
            0,
            &mapRaw);
        if (mappingStatus != kIOReturnSuccess || !mapRaw) {
            rxQueue_.Reset();
            return (mappingStatus == kIOReturnSuccess) ? kIOReturnNoMemory : mappingStatus;
        }

        rxQueue_.map.reset(mapRaw, OSNoRetain);
        rxQueueBase = rxQueue_.BaseAddress();
    }

    if (!isochReceiveContext_) {
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

        if (!isochMem->Initialize(hardware)) {
            ASFW_LOG(Controller, "[Isoch] ❌ StartIsochReceive: Failed to initialize DMA slabs");
            return kIOReturnNoMemory;
        }

        isochReceiveContext_ = ASFW::Isoch::IsochReceiveContext::Create(&hardware, isochMem);

        if (!isochReceiveContext_) {
            ASFW_LOG(Controller, "[Isoch] ❌ StartIsochReceive: Context creation failed");
            return kIOReturnNoMemory;
        }
        ASFW_LOG(Controller, "[Isoch] ✅ provisioned Isoch Context with Dedicated Memory");
    }

    isochReceiveContext_->SetExternalSyncBridge(&externalSyncBridge_);

    auto result = isochReceiveContext_->Configure(channel, 0);
    if (result != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[Isoch] ❌ Failed to Configure IR Context: 0x%x", result);
        rxQueue_.Reset();
        return result;
    }

    isochReceiveContext_->SetSharedRxQueue(rxQueueBase, rxQueueBase ? rxQueueBytes : 0);

    result = isochReceiveContext_->Start();
    if (result != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[Isoch] ❌ Failed to Start IR Context: 0x%x", result);
        isochReceiveContext_->SetSharedRxQueue(nullptr, 0);
        rxQueue_.Reset();
        return result;
    }

    ASFW_LOG(Controller, "[Isoch] ✅ Started IR Context 0 for Channel %u!", channel);

    return kIOReturnSuccess;
}

kern_return_t IsochService::StopReceive() {
    if (!isochReceiveContext_) {
        return kIOReturnNotReady;
    }

    isochReceiveContext_->Stop();
    isochReceiveContext_->SetSharedRxQueue(nullptr, 0);
    rxQueue_.Reset();
    ASFW_LOG(Controller, "[Isoch] Stopped IR Context 0");
    return kIOReturnSuccess;
}

kern_return_t IsochService::StartTransmit(uint8_t channel,
                                          HardwareInterface& hardware,
                                          uint8_t sid,
                                          uint32_t streamModeRaw,
                                          uint32_t pcmChannels,
                                          uint32_t am824Slots,
                                          IOBufferMemoryDescriptor* txQueueMemory,
                                          uint64_t txQueueBytes,
                                          void* zeroCopyBase,
                                          uint64_t zeroCopyBytes,
                                          uint32_t zeroCopyFrames) {

    if (isochTransmitContext_ &&
        isochTransmitContext_->GetState() == ASFW::Isoch::ITState::Running) {
        ASFW_LOG(Controller, "[Isoch] IT already running; StartTransmit is idempotent");
        if (txQueueMemory) {
            txQueueMemory->release();
        }
        return kIOReturnSuccess;
    }

    txQueue_.Reset();

    void* txQueueBase = nullptr;
    if (txQueueMemory && txQueueBytes > 0) {
        txQueue_.memory.reset(txQueueMemory, OSNoRetain);
        txQueue_.bytes = txQueueBytes;

        IOMemoryMap* mapRaw = nullptr;
        const kern_return_t mappingStatus = txQueueMemory->CreateMapping(
            kIOMemoryMapCacheModeDefault,
            0,
            0,
            0,
            0,
            &mapRaw);
        if (mappingStatus != kIOReturnSuccess || !mapRaw) {
            txQueue_.Reset();
            return (mappingStatus == kIOReturnSuccess) ? kIOReturnNoMemory : mappingStatus;
        }

        txQueue_.map.reset(mapRaw, OSNoRetain);
        txQueueBase = txQueue_.BaseAddress();
    }

    if (!isochTransmitContext_) {
        ASFW::Isoch::Memory::IsochMemoryConfig config;
        config.numDescriptors = ASFW::Isoch::IsochTransmitContext::kRingBlocks;
        config.packetSizeBytes = ASFW::Isoch::IsochTransmitContext::kMaxPacketSize;
        config.descriptorAlignment = ASFW::Isoch::IsochTransmitContext::kOHCIPageSize;
        config.payloadPageAlignment = 16384;

        auto isochMem = ASFW::Isoch::Memory::IsochDMAMemoryManager::Create(config);
        if (!isochMem) {
            ASFW_LOG(Controller, "[Isoch] ❌ StartIsochTransmit: Failed to create Memory Manager");
            txQueue_.Reset();
            return kIOReturnNoMemory;
        }

        if (!isochMem->Initialize(hardware)) {
            ASFW_LOG(Controller, "[Isoch] ❌ StartIsochTransmit: Failed to initialize DMA slabs");
            txQueue_.Reset();
            return kIOReturnNoMemory;
        }

        isochTransmitContext_ = ASFW::Isoch::IsochTransmitContext::Create(&hardware, isochMem);

        if (!isochTransmitContext_) {
            ASFW_LOG(Controller, "[Isoch] ❌ StartIsochTransmit: Context creation failed");
            txQueue_.Reset();
            return kIOReturnNoMemory;
        }
        ASFW_LOG(Controller, "[Isoch] ✅ provisioned IT Context with Dedicated Memory");
    }

    uint32_t startTargetFill = ASFW::Isoch::Config::kTxBufferProfile.startWaitTargetFrames;
    isochTransmitContext_->SetSharedTxQueue(txQueueBase, txQueueBase ? txQueueBytes : 0);
    if (txQueueBase && txQueueBytes > 0) {
        ASFW_LOG(Controller, "[Isoch] Wired shared TX queue to IT context (bytes=%llu)",
                 txQueueBytes);
    }

    if (zeroCopyBase && zeroCopyBytes > 0 && zeroCopyFrames > 0) {
        isochTransmitContext_->SetZeroCopyOutputBuffer(zeroCopyBase, zeroCopyBytes, zeroCopyFrames);
        uint32_t target = (zeroCopyFrames * 5) / 8;
        if (target < 8) target = 8;
        startTargetFill = target;
        ASFW_LOG(Controller,
                 "[Isoch] ✅ ZERO-COPY wired! AudioBuffer base=%p bytes=%llu frames=%u targetFill=%u",
                 zeroCopyBase,
                 zeroCopyBytes,
                 zeroCopyFrames,
                 startTargetFill);
    } else {
        isochTransmitContext_->SetZeroCopyOutputBuffer(nullptr, 0, 0);
    }

    if (isochTransmitContext_->SharedTxCapacityFrames() == 0) {
        ASFW_LOG(Controller, "[Isoch] ❌ StartTransmit blocked: shared TX queue metadata missing");
        isochTransmitContext_->SetZeroCopyOutputBuffer(nullptr, 0, 0);
        isochTransmitContext_->SetSharedTxQueue(nullptr, 0);
        txQueue_.Reset();
        return kIOReturnNotReady;
    }

    if (!isochReceiveContext_ ||
        isochReceiveContext_->GetState() != ASFW::Isoch::IRPolicy::State::Running) {
        ASFW_LOG(Controller, "[Isoch] ❌ StartTransmit blocked: IR context is not running");
        isochTransmitContext_->SetZeroCopyOutputBuffer(nullptr, 0, 0);
        isochTransmitContext_->SetSharedTxQueue(nullptr, 0);
        txQueue_.Reset();
        return kIOReturnNotReady;
    }

    constexpr uint32_t kSytGateTimeoutMs = 500;
    constexpr uint32_t kSytGatePollMs = 5;
    bool sytClockEstablished = false;
    for (uint32_t waitedMs = 0; waitedMs < kSytGateTimeoutMs; waitedMs += kSytGatePollMs) {
        if (externalSyncBridge_.clockEstablished.load(std::memory_order_acquire)) {
            sytClockEstablished = true;
            break;
        }
        IOSleep(kSytGatePollMs);
    }
    if (!sytClockEstablished) {
        const uint32_t seq = externalSyncBridge_.updateSeq.load(std::memory_order_acquire);
        const uint32_t packed = externalSyncBridge_.lastPackedRx.load(std::memory_order_acquire);
        const uint16_t lastSyt = ASFW::Isoch::Core::ExternalSyncBridge::UnpackSYT(packed);
        const uint8_t lastFdf = ASFW::Isoch::Core::ExternalSyncBridge::UnpackFDF(packed);
        const uint8_t lastDbs = ASFW::Isoch::Core::ExternalSyncBridge::UnpackDBS(packed);
        const uint64_t lastTicks = externalSyncBridge_.lastUpdateHostTicks.load(std::memory_order_acquire);
        uint64_t ageMs = 0;
        if (lastTicks != 0) {
            const uint64_t nowTicks = mach_absolute_time();
            if (nowTicks >= lastTicks) {
                ageMs = ASFW::Timing::hostTicksToNanos(nowTicks - lastTicks) / 1'000'000ULL;
            }
        }
        ASFW_LOG(Controller,
                 "[Isoch] ❌ StartTransmit timeout: missing established IR SYT clock (waited %ums seq=%u syt=0x%04x fdf=0x%02x dbs=%u ageMs=%llu active=%d established=%d)",
                 kSytGateTimeoutMs,
                 seq,
                 lastSyt,
                 lastFdf,
                 lastDbs,
                 ageMs,
                 externalSyncBridge_.active.load(std::memory_order_acquire),
                 externalSyncBridge_.clockEstablished.load(std::memory_order_acquire));
        isochTransmitContext_->SetZeroCopyOutputBuffer(nullptr, 0, 0);
        isochTransmitContext_->SetSharedTxQueue(nullptr, 0);
        txQueue_.Reset();
        return kIOReturnTimeout;
    }

    isochTransmitContext_->SetExternalSyncBridge(&externalSyncBridge_);

    auto result = isochTransmitContext_->Configure(channel,
                                                   sid,
                                                   streamModeRaw,
                                                   pcmChannels,
                                                   am824Slots);
    if (result != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[Isoch] ❌ Failed to Configure IT Context: 0x%x", result);
        isochTransmitContext_->SetZeroCopyOutputBuffer(nullptr, 0, 0);
        isochTransmitContext_->SetSharedTxQueue(nullptr, 0);
        txQueue_.Reset();
        return result;
    }

    const auto& txProfile = ASFW::Isoch::Config::kTxBufferProfile;
    ASFW_LOG(Controller,
             "[Isoch] IT TX profile=%{public}s startWait=%u startupPrimeLimit=%u legacy(target=%u max=%u chunks=%u)",
             txProfile.name,
             txProfile.startWaitTargetFrames,
             txProfile.startupPrimeLimitFrames,
             txProfile.legacyRbTargetFrames,
             txProfile.legacyRbMaxFrames,
             txProfile.legacyMaxChunksPerRefill);

    uint32_t fillLevel = 0;
    uint32_t targetFill = startTargetFill;
    const uint32_t queueCapacity = isochTransmitContext_->SharedTxCapacityFrames();
    if (queueCapacity > 0 && targetFill > queueCapacity) {
        ASFW_LOG(Controller, "[Isoch] IT start wait target clamped %u -> %u (queueCapacity)",
                 targetFill, queueCapacity);
        targetFill = queueCapacity;
    }
    const int maxWaitMs = 100;

    ASFW_LOG(Controller, "[Isoch] IT start wait targetFill=%u (zeroCopy=%{public}s)",
             targetFill, isochTransmitContext_->IsZeroCopyEnabled() ? "YES" : "NO");

    for (int waitMs = 0; waitMs < maxWaitMs; waitMs += 5) {
        fillLevel = isochTransmitContext_->SharedTxFillLevelFrames();
        if (fillLevel >= targetFill) {
            break;
        }
        IOSleep(5);
    }

    result = isochTransmitContext_->Start();
    if (result != kIOReturnSuccess) {
        ASFW_LOG(Controller, "[Isoch] Failed to Start IT Context: 0x%x", result);
        isochTransmitContext_->SetZeroCopyOutputBuffer(nullptr, 0, 0);
        isochTransmitContext_->SetSharedTxQueue(nullptr, 0);
        txQueue_.Reset();
        return result;
    }

    ASFW_LOG(Controller, "[Isoch] ✅ Started IT Context for Channel %u!", channel);

    return kIOReturnSuccess;
}

kern_return_t IsochService::StopTransmit() {
    if (!isochTransmitContext_) {
        return kIOReturnNotReady;
    }

    isochTransmitContext_->Stop();
    isochTransmitContext_->SetZeroCopyOutputBuffer(nullptr, 0, 0);
    isochTransmitContext_->SetSharedTxQueue(nullptr, 0);
    txQueue_.Reset();
    ASFW_LOG(Controller, "[Isoch] Stopped IT Context");
    return kIOReturnSuccess;
}

kern_return_t IsochService::StartDuplex(const IsochDuplexStartParams& params,
                                       HardwareInterface& hardware) {
    // Ownership: callers pass retained IOBufferMemoryDescriptor* values (from
    // ASFWAudioNub::Copy*QueueMemory). IsochService consumes them regardless of
    // outcome, releasing on all early-return paths.
    IOBufferMemoryDescriptor* rxQueueMemory = params.rxQueueMemory;
    IOBufferMemoryDescriptor* txQueueMemory = params.txQueueMemory;

    if (params.guid == 0) {
        if (rxQueueMemory) rxQueueMemory->release();
        if (txQueueMemory) txQueueMemory->release();
        return kIOReturnBadArgument;
    }

    if (activeGuid_ != 0 && activeGuid_ != params.guid) {
        // Transport layer is currently global. Control-plane enforces single-device too.
        if (rxQueueMemory) rxQueueMemory->release();
        if (txQueueMemory) txQueueMemory->release();
        return kIOReturnBusy;
    }

    const kern_return_t krRx = StartReceive(params.irChannel,
                                           hardware,
                                           rxQueueMemory,
                                           params.rxQueueBytes);
    if (krRx != kIOReturnSuccess) {
        if (txQueueMemory) txQueueMemory->release();
        return krRx;
    }

    const uint32_t streamModeRaw = static_cast<uint32_t>(params.streamMode);
    const kern_return_t krTx = StartTransmit(params.itChannel,
                                            hardware,
                                            params.sid,
                                            streamModeRaw,
                                            params.hostOutputPcmChannels,
                                            params.hostToDeviceAm824Slots,
                                            txQueueMemory,
                                            params.txQueueBytes,
                                            params.zeroCopyBase,
                                            params.zeroCopyBytes,
                                            params.zeroCopyFrames);
    if (krTx != kIOReturnSuccess) {
        StopReceive();
        return krTx;
    }

    activeGuid_ = params.guid;
    return kIOReturnSuccess;
}

kern_return_t IsochService::StopDuplex(uint64_t guid) {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }

    if (activeGuid_ != 0 && activeGuid_ != guid) {
        return kIOReturnBusy;
    }

    (void)StopTransmit();
    (void)StopReceive();
    externalSyncBridge_.Reset();

    activeGuid_ = 0;
    return kIOReturnSuccess;
}

void IsochService::StopAll() {
    if (isochReceiveContext_) {
        isochReceiveContext_->Stop();
        isochReceiveContext_->SetSharedRxQueue(nullptr, 0);
        isochReceiveContext_.reset();
    }
    rxQueue_.Reset();
    if (isochTransmitContext_) {
        isochTransmitContext_->Stop();
        isochTransmitContext_->SetZeroCopyOutputBuffer(nullptr, 0, 0);
        isochTransmitContext_->SetSharedTxQueue(nullptr, 0);
        isochTransmitContext_.reset();
    }
    txQueue_.Reset();
    externalSyncBridge_.Reset();
    activeGuid_ = 0;
}

} // namespace ASFW::Driver
