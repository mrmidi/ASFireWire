// IsochService.cpp
// ASFW - Isochronous Service (orchestrator for IT/IR contexts)

#include "IsochService.hpp"
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

    ASFW_LOG(Isoch, "IsochService: Starting IR on channel %u (Direct-Only)", channel);
    const kern_return_t startKr = isochReceiveContext_->Start();
    return startKr;
}

kern_return_t IsochService::StopReceive() {
    if (isochReceiveContext_) {
        isochReceiveContext_->Stop();
        isochReceiveContext_->SetDirectAudioBindingSource(nullptr);
        // Safe after Stop(): Poll no longer runs, so no callback is in flight.
        isochReceiveContext_->SetCallback(nullptr);
    }

    if (dvCaptureActive_) {
        dvSink_.Detach();
        dvRing_.Reset();
        dvCaptureActive_ = false;
        ASFW_LOG(Isoch, "IsochService: DV capture stopped");
    }

    return kIOReturnSuccess;
}

// ============================================================================
// DV capture (minimal IEC 61883-2 tap; see Receive/DVCaptureSink.hpp)
// ============================================================================

kern_return_t IsochService::StartDVCapture(uint8_t channel, HardwareInterface& hardware) {
    if (dvCaptureActive_) {
        ASFW_LOG(Isoch, "IsochService: DV capture already active; StartDVCapture is idempotent");
        return kIOReturnSuccess;
    }

    if (isochReceiveContext_ &&
        isochReceiveContext_->GetState() == ASFW::Isoch::IRPolicy::State::Running) {
        ASFW_LOG(Isoch, "IsochService: StartDVCapture blocked: IR context busy (audio receive running)");
        return kIOReturnBusy;
    }

    // ~3.9MB ring = 8192 DIF chunks ≈ 1.1s of DV at 3.6MB/s.
    constexpr uint32_t kDVRingRecords = 8192;
    const uint64_t ringBytes = ASFW::Isoch::Rx::DVCaptureSink::RequiredBytes(kDVRingRecords);

    IOBufferMemoryDescriptor* memRaw = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionOutIn,
                                                        ringBytes, 64, &memRaw);
    if (kr != kIOReturnSuccess || !memRaw) {
        ASFW_LOG(Isoch, "IsochService: StartDVCapture ring allocation failed: 0x%x", kr);
        return kr ? kr : kIOReturnNoMemory;
    }
    kr = memRaw->SetLength(ringBytes);
    if (kr != kIOReturnSuccess) {
        memRaw->release();
        return kr;
    }

    dvRing_.Reset();
    dvRing_.memory = Common::AdoptRetained(memRaw);
    dvRing_.bytes = ringBytes;

    kr = Common::CreateSharedMapping(dvRing_.memory, dvRing_.map);
    if (kr != kIOReturnSuccess) {
        dvRing_.Reset();
        return kr;
    }

    if (!dvSink_.InitializeAndAttach(dvRing_.BaseAddress(), ringBytes, kDVRingRecords)) {
        ASFW_LOG(Isoch, "IsochService: StartDVCapture sink init failed");
        dvRing_.Reset();
        return kIOReturnInternalError;
    }

    auto* sink = &dvSink_;
    kr = StartReceive(channel, hardware, /*bindingSource=*/nullptr,
                      ASFW::Encoding::AudioWireFormat::kAM824, /*am824Slots=*/0,
                      [sink](std::span<const uint8_t> data, uint32_t status,
                             uint64_t /*timestamp*/) {
                          sink->OnPacket(data.data(), data.size(), status);
                      });
    if (kr != kIOReturnSuccess) {
        dvSink_.Detach();
        dvRing_.Reset();
        return kr;
    }

    dvCaptureActive_ = true;
    ASFW_LOG(Isoch, "IsochService: DV capture started on channel %u (ring=%llu bytes)",
             channel, ringBytes);
    return kIOReturnSuccess;
}

kern_return_t IsochService::StopDVCapture() {
    if (!dvCaptureActive_) {
        return kIOReturnSuccess;
    }
    return StopReceive();
}

kern_return_t IsochService::CopyDVCaptureMemory(uint64_t* options, IOMemoryDescriptor** memory) const {
    if (!memory) {
        return kIOReturnBadArgument;
    }
    if (!dvCaptureActive_ || !dvRing_.memory) {
        return kIOReturnNotReady;
    }
    if (options) {
        *options = 0;
    }
    dvRing_.memory->retain();
    *memory = dvRing_.memory.get();
    return kIOReturnSuccess;
}

kern_return_t IsochService::StartTransmit(uint8_t channel,
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

    ASFW_LOG(Isoch, "IsochService: Starting IT on channel %u (Direct-Only)", channel);
    return isochTransmitContext_->Start();
}

kern_return_t IsochService::StopTransmit() {
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
    txPayloadSlab_ = OSSharedPtr<IOBufferMemoryDescriptor>(payloadDescriptor, OSNoRetain);

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
    txMetadataRing_ = OSSharedPtr<IOBufferMemoryDescriptor>(metadataDescriptor, OSNoRetain);

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
    txControlBlock_ = OSSharedPtr<IOBufferMemoryDescriptor>(controlDescriptor, OSNoRetain);

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
