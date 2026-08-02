// IsochService.cpp
// ASFW - Isochronous Service (orchestrator for IT/IR contexts)

#include "IsochService.hpp"
#include "../Common/DriverKitOwnership.hpp"
#include "../Logging/Logging.hpp"
#ifndef ASFW_HOST_TEST
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#endif
#include "Memory/IsochDMAMemoryManager.hpp"
#include "../Shared/Isoch/IsochAudioTransport.hpp"

namespace ASFW::Driver {

using namespace ASFW::Isoch;

kern_return_t IsochService::StartReceive(uint8_t channel,
                                         HardwareInterface& hardware,
                                         ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                                         ASFW::Encoding::AudioWireFormat wireFormat,
                                         uint32_t am824Slots,
                                         ASFW::Isoch::IsochReceiveCallback packetCallback) {
    const kern_return_t prepareKr =
        PrepareReceive(channel,
                       hardware,
                       bindingSource,
                       wireFormat,
                       am824Slots,
                       std::move(packetCallback));
    if (prepareKr != kIOReturnSuccess) {
        return prepareKr;
    }
    return StartPreparedReceive();
}

kern_return_t IsochService::PrepareReceive(
    uint8_t channel,
    HardwareInterface& hardware,
    ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
    ASFW::Encoding::AudioWireFormat wireFormat,
    uint32_t am824Slots,
    ASFW::Isoch::IsochReceiveCallback packetCallback) {
    if (!isochReceiveContext_) {
        ASFW::Isoch::Memory::IsochMemoryConfig config;
        config.numDescriptors = ASFW::Isoch::IsochReceiveContext::kNumDescriptors;
        config.packetSizeBytes = ASFW::Isoch::IsochReceiveContext::kMaxPacketSize;
        config.descriptorAlignment = 16;
        config.payloadPageAlignment = 16384;

        auto isochMem = ASFW::Isoch::Memory::IsochDMAMemoryManager::Create(config);
        if (!isochMem) {
            ASFW_LOG(Isoch, "IsochService: Failed to create RX DMA memory manager");
            return kIOReturnNoMemory;
        }

        if (!isochMem->Initialize(hardware)) {
            ASFW_LOG(Isoch, "IsochService: Failed to initialize RX DMA memory");
            return kIOReturnNoMemory;
        }

        isochReceiveContext_ = IsochReceiveContext::Create(&hardware, isochMem);
        if (!isochReceiveContext_) {
            ASFW_LOG(Isoch, "IsochService: Failed to create IR context");
            return kIOReturnNoMemory;
        }
        RefreshReceiveTimingLossCallback();
        isochReceiveContext_->SetZtsAnchorReadyCallback(
            ztsAnchorReadyCallback_);
        isochReceiveContext_->SetReplayReadyCallback([this]() {
            StartDeferredTransmitIfReady();
        });
    }

    isochReceiveContext_->SetDirectAudioBindingSource(bindingSource);

    const kern_return_t kr = isochReceiveContext_->Configure(channel, 0, wireFormat, am824Slots);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Isoch, "IsochService: IR Configure failed: 0x%08x", kr);
        return kr;
    }

    // Install (or clear) the per-packet callback before Start so Poll never
    // races a std::function assignment.
    isochReceiveContext_->SetCallback(std::move(packetCallback));

    ASFW_LOG(Isoch, "IsochService: Prepared IR on channel %u (Direct-Only)", channel);
    return kIOReturnSuccess;
}

kern_return_t IsochService::StartPreparedReceive() {
    if (!isochReceiveContext_) {
        return kIOReturnNotReady;
    }
    ASFW_LOG(Isoch, "IsochService: Starting prepared IR (Direct-Only)");
    return isochReceiveContext_->Start();
}

kern_return_t IsochService::StartPacketReceive(
    uint8_t channel,
    HardwareInterface& hardware,
    ASFW::Isoch::IsochReceiveCallback packetCallback) {
    if (!packetCallback) {
        return kIOReturnBadArgument;
    }

    if (!isochReceiveContext_) {
        ASFW::Isoch::Memory::IsochMemoryConfig config;
        config.numDescriptors = ASFW::Isoch::IsochReceiveContext::kNumDescriptors;
        config.packetSizeBytes = ASFW::Isoch::IsochReceiveContext::kMaxPacketSize;
        config.descriptorAlignment = 16;
        config.payloadPageAlignment = 16384;

        auto isochMem = ASFW::Isoch::Memory::IsochDMAMemoryManager::Create(config);
        if (!isochMem || !isochMem->Initialize(hardware)) {
            return kIOReturnNoMemory;
        }
        isochReceiveContext_ = IsochReceiveContext::Create(&hardware, isochMem);
        if (!isochReceiveContext_) {
            return kIOReturnNoMemory;
        }
        RefreshReceiveTimingLossCallback();
    }

    if (isochReceiveContext_->GetState() !=
        ASFW::Isoch::IRPolicy::State::Stopped) {
        return kIOReturnBusy;
    }

    const kern_return_t configureKr =
        isochReceiveContext_->ConfigurePacketReceive(channel, 0);
    if (configureKr != kIOReturnSuccess) {
        return configureKr;
    }
    isochReceiveContext_->SetCallback(std::move(packetCallback));
    const kern_return_t startKr = isochReceiveContext_->Start();
    if (startKr != kIOReturnSuccess) {
        isochReceiveContext_->SetCallback(nullptr);
    }
    return startKr;
}

kern_return_t IsochService::StopReceive() {
    if (isochReceiveContext_) {
        isochReceiveContext_->Stop();
        isochReceiveContext_->SetDirectAudioBindingSource(nullptr);
        // Safe after Stop(): Poll no longer runs, so no callback is in flight.
        isochReceiveContext_->SetCallback(nullptr);
    }

    return kIOReturnSuccess;
}

kern_return_t IsochService::StartTransmit(uint8_t channel,
                                          HardwareInterface& hardware,
                                          uint8_t sid) {
    const kern_return_t prepareKr = PrepareTransmit(channel, hardware, sid);
    if (prepareKr != kIOReturnSuccess) {
        return prepareKr;
    }
    return StartPreparedTransmit();
}

kern_return_t IsochService::PrepareTransmit(uint8_t channel,
                                            HardwareInterface& hardware,
                                            uint8_t sid) {
    if (!isochTransmitContext_) {
        ASFW::Isoch::Memory::IsochMemoryConfig config;
        config.numDescriptors = ASFW::Isoch::Tx::Layout::kRingBlocks;
        config.packetSizeBytes = 0;
        config.descriptorAlignment = ASFW::Isoch::Tx::Layout::kOHCIPageSize;
        config.payloadPageAlignment = 16384;
        config.allocatePayloadSlab = false;

        auto isochMem = ASFW::Isoch::Memory::IsochDMAMemoryManager::Create(config);
        if (!isochMem) {
            ASFW_LOG(Isoch, "IsochService: Failed to create TX DMA memory manager");
            return kIOReturnNoMemory;
        }

        if (!isochMem->Initialize(hardware)) {
            ASFW_LOG(Isoch, "IsochService: Failed to initialize TX DMA memory");
            return kIOReturnNoMemory;
        }

        isochTransmitContext_ = IsochTransmitContext::Create(&hardware, isochMem);
        if (!isochTransmitContext_) {
            ASFW_LOG(Isoch, "IsochService: Failed to create IT context");
            return kIOReturnNoMemory;
        }
        isochTransmitContext_->SetTxPreparationCallback(
            txPreparationCallback_);
    }

    const kern_return_t kr = isochTransmitContext_->Configure(channel, sid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Isoch, "IsochService: IT Configure failed: 0x%08x", kr);
        return kr;
    }

    if (txPayloadSlab_ && txMetadataRing_ && txControlBlock_) {
        const kern_return_t memKr = isochTransmitContext_->SetSharedMemoryDescriptors(
            txPayloadSlab_.get(),
            txMetadataRing_.get(),
            txControlBlock_.get(),
            interruptInterval_,
            ASFW::IsochTransport::AudioTimingGeometry::kHalZeroTimestampPeriodFrames);
        if (memKr != kIOReturnSuccess) {
            ASFW_LOG(Isoch,
                     "IsochService: IT shared-memory setup failed: 0x%08x",
                     memKr);
            return memKr;
        }
    }

    ASFW_LOG(Isoch, "IsochService: Prepared IT on channel %u (Direct-Only)", channel);
    return kIOReturnSuccess;
}

kern_return_t IsochService::StartPreparedTransmit() {
    if (!isochTransmitContext_) {
        return kIOReturnNotReady;
    }
    if (isochReceiveContext_ &&
        isochReceiveContext_->GetState() ==
            ASFW::Isoch::IRPolicy::State::Running &&
        !isochReceiveContext_->IsReplayEstablished()) {
        txStartPending_ = true;
        ASFW_LOG(
            Isoch,
            "IsochService: IT RUN deferred until IR cadence/replay is established");
        return kIOReturnSuccess;
    }
    txStartPending_ = false;
    ASFW_LOG(Isoch, "IsochService: Starting prepared IT (Direct-Only)");
    return isochTransmitContext_->Start();
}

kern_return_t IsochService::StopTransmit() {
    txStartPending_ = false;
    if (isochTransmitContext_) {
        isochTransmitContext_->Stop();
    }
    return kIOReturnSuccess;
}

kern_return_t IsochService::BeginSplitDuplex(uint64_t guid) {
    const kern_return_t kr = ClaimDuplexGuid(guid);
    if (kr != kIOReturnSuccess) return kr;
    
    reserved_.Reset();
    return kIOReturnSuccess;
}

kern_return_t IsochService::ReservePlaybackResources(uint64_t guid,
                                                     IRM::IRMClient& irmClient,
                                                     uint8_t channel,
                                                     uint32_t bandwidthUnits) {
    if (activeGuid_ != guid) return kIOReturnNotPrivileged;
    
    reserved_.playbackActive = true;
    reserved_.playbackChannel = channel;
    reserved_.playbackBandwidthUnits = bandwidthUnits;
    return kIOReturnSuccess;
}

kern_return_t IsochService::ReserveCaptureResources(uint64_t guid,
                                                    IRM::IRMClient& irmClient,
                                                    uint8_t channel,
                                                    uint32_t bandwidthUnits) {
    if (activeGuid_ != guid) return kIOReturnNotPrivileged;
    
    reserved_.captureActive = true;
    reserved_.captureChannel = channel;
    reserved_.captureBandwidthUnits = bandwidthUnits;
    return kIOReturnSuccess;
}

void IsochService::StopAll() {
    StopReceive();
    StopTransmit();
    reserved_.Reset();
    activeGuid_ = 0;
}

void IsochService::SetTimingLossCallback(TimingLossCallback callback) noexcept {
    timingLossCallback_ = std::move(callback);
}

void IsochService::SetTxPreparationCallback(
    TxPreparationCallback callback) noexcept {
    txPreparationCallback_ = std::move(callback);
    if (isochTransmitContext_) {
        isochTransmitContext_->SetTxPreparationCallback(
            txPreparationCallback_);
    }
}

void IsochService::SetZtsAnchorReadyCallback(
    ZtsAnchorReadyCallback callback) noexcept {
    ztsAnchorReadyCallback_ = std::move(callback);
    if (isochReceiveContext_) {
        isochReceiveContext_->SetZtsAnchorReadyCallback(
            ztsAnchorReadyCallback_);
    }
}

kern_return_t IsochService::ClaimDuplexGuid(uint64_t guid) {
    if (activeGuid_ != 0 && activeGuid_ != guid) {
        ASFW_LOG(Isoch, "IsochService: GUID conflict 0x%llx (active: 0x%llx)",
                 guid, activeGuid_);
        return kIOReturnBusy;
    }
    activeGuid_ = guid;
    return kIOReturnSuccess;
}

void IsochService::RefreshReceiveTimingLossCallback() noexcept {
    if (isochReceiveContext_) {
        isochReceiveContext_->SetTimingLossCallback([this]() {
            OnReceiveTimingLossDetected();
        });
    }
}

void IsochService::OnReceiveTimingLossDetected() noexcept {
    if (timingLossCallback_ && activeGuid_ != 0) {
        timingLossCallback_(activeGuid_);
    }
}

void IsochService::StartDeferredTransmitIfReady() noexcept {
    if (!txStartPending_ || !isochTransmitContext_ ||
        !isochReceiveContext_ ||
        !isochReceiveContext_->IsReplayEstablished()) {
        return;
    }

    txStartPending_ = false;
    ASFW_LOG(
        Isoch,
        "IsochService: IR replay established; starting deferred IT");
    const kern_return_t status = isochTransmitContext_->Start();
    if (status != kIOReturnSuccess) {
        ASFW_LOG(
            Isoch,
            "IsochService: deferred IT start failed: 0x%08x",
            status);
        OnReceiveTimingLossDetected();
    }
}

kern_return_t IsochService::AllocateTxIsochResources(
    uint32_t numSlots,
    uint32_t maxPacketBytes,
    uint32_t interruptInterval,
    IOMemoryDescriptor** outPayloadSlab,
    IOMemoryDescriptor** outMetadataRing,
    IOMemoryDescriptor** outControlBlock)
{
    if (!outPayloadSlab || !outMetadataRing || !outControlBlock) {
        return kIOReturnBadArgument;
    }
    *outPayloadSlab = nullptr;
    *outMetadataRing = nullptr;
    *outControlBlock = nullptr;

    FreeTxIsochResources();

    // 1. Allocate payload slab (page-aligned)
    const size_t payloadSlabBytes = static_cast<size_t>(numSlots) * maxPacketBytes;
    IOBufferMemoryDescriptor* payloadDescriptor = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionInOut,
        payloadSlabBytes,
        4096,
        &payloadDescriptor);
    if (kr != kIOReturnSuccess || !payloadDescriptor) {
        ASFW_LOG(Isoch, "IsochService: Failed to allocate payload slab: 0x%08x", kr);
        FreeTxIsochResources();
        return (kr == kIOReturnSuccess) ? kIOReturnNoMemory : kr;
    }
    txPayloadSlab_ = ASFW::Common::AdoptRetained(payloadDescriptor);

    // 2. Allocate metadata ring (cacheline aligned)
    const size_t metadataRingBytes = static_cast<size_t>(numSlots) * sizeof(ASFW::IsochTransport::TxPacketMeta);
    IOBufferMemoryDescriptor* metadataDescriptor = nullptr;
    kr = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionInOut,
        metadataRingBytes,
        64,
        &metadataDescriptor);
    if (kr != kIOReturnSuccess || !metadataDescriptor) {
        ASFW_LOG(Isoch, "IsochService: Failed to allocate metadata ring: 0x%08x", kr);
        FreeTxIsochResources();
        return (kr == kIOReturnSuccess) ? kIOReturnNoMemory : kr;
    }
    txMetadataRing_ = ASFW::Common::AdoptRetained(metadataDescriptor);

    // 3. Allocate control block (cacheline aligned)
    const size_t controlBlockBytes = sizeof(ASFW::IsochTransport::TxStreamControl);
    IOBufferMemoryDescriptor* controlDescriptor = nullptr;
    kr = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionInOut,
        controlBlockBytes,
        64,
        &controlDescriptor);
    if (kr != kIOReturnSuccess || !controlDescriptor) {
        ASFW_LOG(Isoch, "IsochService: Failed to allocate control block: 0x%08x", kr);
        FreeTxIsochResources();
        return (kr == kIOReturnSuccess) ? kIOReturnNoMemory : kr;
    }
    txControlBlock_ = ASFW::Common::AdoptRetained(controlDescriptor);

    // Return the descriptors to the caller with retained references
    *outPayloadSlab = txPayloadSlab_.get();
    (*outPayloadSlab)->retain();

    *outMetadataRing = txMetadataRing_.get();
    (*outMetadataRing)->retain();

    *outControlBlock = txControlBlock_.get();
    (*outControlBlock)->retain();

    interruptInterval_ = interruptInterval;

    ASFW_LOG(Isoch, "IsochService: Allocated Tx isoch resources. numSlots=%u slotSize=%u", numSlots, maxPacketBytes);
    return kIOReturnSuccess;
}

kern_return_t IsochService::FreeTxIsochResources()
{
    txPayloadSlab_ = nullptr;
    txMetadataRing_ = nullptr;
    txControlBlock_ = nullptr;
    ASFW_LOG(Isoch, "IsochService: Freed Tx isoch resources");
    return kIOReturnSuccess;
}

kern_return_t IsochService::GetCycleTimePair(uint64_t* outHostTimeMid, uint32_t* outCycleTimer, HardwareInterface& hardware) {
    if (!outHostTimeMid || !outCycleTimer) {
        return kIOReturnBadArgument;
    }

    const uint32_t cycleTimer = hardware.Read(static_cast<Register32>(Register32::kCycleTimer));
    const uint64_t hostTime = mach_absolute_time();

    *outHostTimeMid = hostTime;
    *outCycleTimer = cycleTimer;
    return kIOReturnSuccess;
}

} // namespace ASFW::Driver
