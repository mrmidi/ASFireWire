// IsochService.cpp
// ASFW - Isochronous Service (orchestrator for IT/IR contexts)

#include "IsochService.hpp"
#include "../Logging/Logging.hpp"
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include "Memory/IsochDMAMemoryManager.hpp"
#include "../Shared/Isoch/IsochAudioTransport.hpp"

namespace ASFW::Driver {

using namespace ASFW::Isoch;

kern_return_t IsochService::StartReceive(uint8_t channel,
                                         HardwareInterface& hardware,
                                         ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                                         ASFW::Encoding::AudioWireFormat wireFormat,
                                         uint32_t am824Slots) {
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
    }

    isochReceiveContext_->SetDirectAudioBindingSource(bindingSource);

    const kern_return_t kr = isochReceiveContext_->Configure(channel, 0, wireFormat, am824Slots);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Isoch, "IsochService: IR Configure failed: 0x%08x", kr);
        return kr;
    }

    ASFW_LOG(Isoch, "IsochService: Starting IR on channel %u (Direct-Only)", channel);
    const kern_return_t startKr = isochReceiveContext_->Start();
    return startKr;
}

kern_return_t IsochService::StopReceive() {
    if (isochReceiveContext_) {
        isochReceiveContext_->Stop();
        isochReceiveContext_->SetDirectAudioBindingSource(nullptr);
    }
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
            512);
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

kern_return_t IsochService::StartTxStream(uint32_t channel, uint32_t speed, HardwareInterface& hardware) {
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
    }

    const kern_return_t kr = isochTransmitContext_->Configure(channel, 0);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Isoch, "IsochService: StartTxStream: IT Configure failed: 0x%08x", kr);
        return kr;
    }

    // Set memory regions on the transmit context if they were allocated
    if (txPayloadSlab_ && txMetadataRing_ && txControlBlock_) {
        const kern_return_t memKr = isochTransmitContext_->SetSharedMemoryDescriptors(
            txPayloadSlab_.get(), txMetadataRing_.get(), txControlBlock_.get(), interruptInterval_, 512);
        if (memKr != kIOReturnSuccess) {
            ASFW_LOG(Isoch, "IsochService: StartTxStream: SetSharedMemoryDescriptors failed: 0x%08x", memKr);
            return memKr;
        }
    }

    ASFW_LOG(Isoch, "IsochService: Starting IT stream on channel %u speed %u", channel, speed);
    return isochTransmitContext_->Start();
}

kern_return_t IsochService::StopTxStream() {
    if (isochTransmitContext_) {
        isochTransmitContext_->Stop();
    }
    return kIOReturnSuccess;
}

} // namespace ASFW::Driver
