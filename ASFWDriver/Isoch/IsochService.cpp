// IsochService.cpp
// ASFW - Isochronous Service (orchestrator for IT/IR contexts)

#include "IsochService.hpp"
#include "../Logging/Logging.hpp"
#ifndef ASFW_HOST_TEST
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#endif
#include "Core/IsochTxQueue.hpp"
#include "Memory/IsochDMAMemoryManager.hpp"

namespace ASFW::Driver {

using namespace ASFW::Isoch;

kern_return_t
IsochService::StartReceive(uint8_t channel, HardwareInterface& hardware,
                           ASFW::Isoch::IsochReceiveCallback packetCallback) {
    const kern_return_t prepareKr = PrepareReceive(channel, hardware, std::move(packetCallback));
    if (prepareKr != kIOReturnSuccess) {
        return prepareKr;
    }
    return StartPreparedReceive();
}

kern_return_t
IsochService::PrepareReceive(uint8_t channel, HardwareInterface& hardware,
                             ASFW::Isoch::IsochReceiveCallback packetCallback) {
    hardware_ = &hardware;
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
    }

    isochReceiveContext_->SetReceiveConsumer(receiveConsumers_[0]);

    // Primary stream uses hardware context index 0. Payload interpretation is
    // delegated to the installed consumer.
    const kern_return_t kr =
        isochReceiveContext_->Configure(channel, 0);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Isoch, "IsochService: IR Configure failed: 0x%08x", kr);
        return kr;
    }

    // Install (or clear) the per-packet callback before Start so Poll never
    // races a std::function assignment.
    isochReceiveContext_->SetCallback(std::move(packetCallback));

    ASFW_LOG(Isoch, "IsochService: Prepared IR on channel %u", channel);
    return kIOReturnSuccess;
}

kern_return_t IsochService::PrepareReceiveStream(
    uint32_t streamIndex, uint8_t channel, HardwareInterface& hardware) {
    hardware_ = &hardware;
    // Stream 0 is the master; callers use PrepareReceive() for it.
    if (streamIndex == 0 || streamIndex >= kMaxStreamsPerDirection) {
        return kIOReturnBadArgument;
    }

    auto& slot = secondaryReceiveContexts_[streamIndex - 1];
    if (!slot) {
        ASFW::Isoch::Memory::IsochMemoryConfig config;
        config.numDescriptors = ASFW::Isoch::IsochReceiveContext::kNumDescriptors;
        config.packetSizeBytes = ASFW::Isoch::IsochReceiveContext::kMaxPacketSize;
        config.descriptorAlignment = 16;
        config.payloadPageAlignment = 16384;

        auto isochMem = ASFW::Isoch::Memory::IsochDMAMemoryManager::Create(config);
        if (!isochMem || !isochMem->Initialize(hardware)) {
            ASFW_LOG(Isoch, "IsochService: secondary RX DMA memory init failed (stream %u)",
                     streamIndex);
            return kIOReturnNoMemory;
        }

        slot = IsochReceiveContext::Create(&hardware, isochMem);
        if (!slot) {
            ASFW_LOG(Isoch, "IsochService: Failed to create secondary IR context (stream %u)",
                     streamIndex);
            return kIOReturnNoMemory;
        }
    }

    slot->SetReceiveConsumer(receiveConsumers_[streamIndex]);

    // contextIndex == streamIndex routes this stream to its own OHCI IR context.
    const kern_return_t kr =
        slot->Configure(channel, static_cast<uint8_t>(streamIndex));
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Isoch, "IsochService: secondary IR Configure failed (stream %u): 0x%08x",
                 streamIndex, kr);
        return kr;
    }

    ASFW_LOG(Isoch, "IsochService: Prepared secondary IR stream %u on channel %u",
             streamIndex, channel);
    return kIOReturnSuccess;
}

kern_return_t IsochService::StartPreparedReceive() {
    if (!isochReceiveContext_) {
        return kIOReturnNotReady;
    }
    ASFW_LOG(Isoch, "IsochService: Starting prepared IR");
    const kern_return_t kr = isochReceiveContext_->Start();
    if (kr != kIOReturnSuccess) {
        return kr;
    }
    for (auto& ctx : secondaryReceiveContexts_) {
        if (ctx) {
            const kern_return_t skr = ctx->Start();
            if (skr != kIOReturnSuccess) {
                ASFW_LOG(Isoch, "IsochService: secondary IR start failed: 0x%08x", skr);
                UpdateStreamingActiveState();
                return skr;
            }
        }
    }
    UpdateStreamingActiveState();
    return kIOReturnSuccess;
}

kern_return_t IsochService::StartPacketReceive(
    uint8_t channel, HardwareInterface& hardware,
    ASFW::Isoch::IIsochReceiveConsumer* consumer) {
    if (!consumer) {
        return kIOReturnBadArgument;
    }
    if (isochReceiveContext_ &&
        isochReceiveContext_->GetState() != ASFW::Isoch::IRPolicy::State::Stopped) {
        return kIOReturnBusy;
    }

    SetReceiveConsumer(0, consumer);
    const kern_return_t prepareStatus = PrepareReceive(channel, hardware, nullptr);
    if (prepareStatus != kIOReturnSuccess) {
        SetReceiveConsumer(0, nullptr);
        return prepareStatus;
    }

    const kern_return_t startStatus = StartPreparedReceive();
    if (startStatus != kIOReturnSuccess) {
        SetReceiveConsumer(0, nullptr);
    }
    return startStatus;
}

kern_return_t IsochService::StopPacketReceive(
    ASFW::Isoch::IIsochReceiveConsumer* consumer) {
    if (!consumer || receiveConsumers_[0] != consumer) {
        return kIOReturnBadArgument;
    }

    const kern_return_t stopStatus = StopReceive();
    if (stopStatus == kIOReturnSuccess) {
        // StopReceive observed ACTIVE clear, so the transport cannot invoke
        // this caller-owned consumer after it is detached.
        SetReceiveConsumer(0, nullptr);
    }
    return stopStatus;
}

kern_return_t IsochService::StopReceive() {
    kern_return_t result = kIOReturnSuccess;
    if (isochReceiveContext_) {
        const kern_return_t stopStatus = isochReceiveContext_->Stop();
        if (stopStatus == kIOReturnSuccess) {
            // Safe only after ACTIVE has cleared: Poll cannot be in flight.
            isochReceiveContext_->SetCallback(nullptr);
        } else {
            result = stopStatus;
        }
    }
    for (auto& ctx : secondaryReceiveContexts_) {
        if (ctx) {
            const kern_return_t stopStatus = ctx->Stop();
            if (stopStatus == kIOReturnSuccess) {
                ctx->SetCallback(nullptr);
            } else if (result == kIOReturnSuccess) {
                result = stopStatus;
            }
        }
    }

    return result;
}

kern_return_t IsochService::StartTransmit(uint8_t channel, HardwareInterface& hardware,
                                          uint8_t sid) {
    const kern_return_t prepareKr = PrepareTransmit(channel, hardware, sid);
    if (prepareKr != kIOReturnSuccess) {
        return prepareKr;
    }
    return StartPreparedTransmit();
}

kern_return_t IsochService::PrepareTransmit(uint8_t channel, HardwareInterface& hardware,
                                            uint8_t sid) {
    hardware_ = &hardware;
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
        isochTransmitContext_->SetTxPreparationCallback(txPreparationCallback_);
    }

    const kern_return_t kr = isochTransmitContext_->Configure(channel, sid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Isoch, "IsochService: IT Configure failed: 0x%08x", kr);
        return kr;
    }

    if (txPayloadSlab_[0] && txMetadataRing_[0] && txControlBlock_[0]) {
        const kern_return_t memKr = isochTransmitContext_->SetSharedMemoryDescriptors(
            txPayloadSlab_[0].get(), txMetadataRing_[0].get(), txControlBlock_[0].get(),
            interruptInterval_);
        if (memKr != kIOReturnSuccess) {
            ASFW_LOG(Isoch, "IsochService: IT shared-memory setup failed: 0x%08x", memKr);
            return memKr;
        }
    }

    ASFW_LOG(Isoch, "IsochService: Prepared IT on channel %u", channel);
    return kIOReturnSuccess;
}

kern_return_t IsochService::PrepareTransmitStream(uint32_t streamIndex, uint8_t channel,
                                                  HardwareInterface& hardware, uint8_t sid) {
    hardware_ = &hardware;
    // Stream 0 is the master; callers use PrepareTransmit() for it.
    if (streamIndex == 0 || streamIndex >= kMaxStreamsPerDirection) {
        return kIOReturnBadArgument;
    }

    auto& slot = secondaryTransmitContexts_[streamIndex - 1];
    if (!slot) {
        ASFW::Isoch::Memory::IsochMemoryConfig config;
        config.numDescriptors = ASFW::Isoch::Tx::Layout::kRingBlocks;
        config.packetSizeBytes = 0;
        config.descriptorAlignment = ASFW::Isoch::Tx::Layout::kOHCIPageSize;
        config.payloadPageAlignment = 16384;
        config.allocatePayloadSlab = false;

        auto isochMem = ASFW::Isoch::Memory::IsochDMAMemoryManager::Create(config);
        if (!isochMem || !isochMem->Initialize(hardware)) {
            ASFW_LOG(Isoch, "IsochService: secondary TX DMA memory init failed (stream %u)",
                     streamIndex);
            return kIOReturnNoMemory;
        }

        slot = IsochTransmitContext::Create(&hardware, isochMem);
        if (!slot) {
            ASFW_LOG(Isoch, "IsochService: Failed to create secondary IT context (stream %u)",
                     streamIndex);
            return kIOReturnNoMemory;
        }
        // No TX-preparation callback on secondaries; the master drives refill.
    }

    // Each secondary stream runs on its own OHCI IT context (== streamIndex) so
    // it does not collide with the master (context 0) on the hardware registers.
    slot->SetContextIndex(static_cast<uint8_t>(streamIndex));

    const kern_return_t kr = slot->Configure(channel, sid);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Isoch, "IsochService: secondary IT Configure failed (stream %u): 0x%08x",
                 streamIndex, kr);
        return kr;
    }
    // Wire this stream's own opaque payload slab and queue metadata so the
    // context can DMA packets prepared by any content producer.
    if (txPayloadSlab_[streamIndex] && txMetadataRing_[streamIndex] &&
        txControlBlock_[streamIndex]) {
        const kern_return_t memKr = slot->SetSharedMemoryDescriptors(
            txPayloadSlab_[streamIndex].get(), txMetadataRing_[streamIndex].get(),
            txControlBlock_[streamIndex].get(), interruptInterval_);
        if (memKr != kIOReturnSuccess) {
            ASFW_LOG(Isoch,
                     "IsochService: secondary IT shared-memory setup failed (stream %u): 0x%08x",
                     streamIndex, memKr);
            return memKr;
        }
    } else {
        ASFW_LOG(Isoch, "IsochService: secondary IT stream %u has no shared slab — not startable",
                 streamIndex);
        return kIOReturnNotReady;
    }

    ASFW_LOG(Isoch, "IsochService: Prepared secondary IT stream %u on channel %u", streamIndex,
             channel);
    return kIOReturnSuccess;
}

kern_return_t IsochService::StartPreparedTransmit() {
    if (!isochTransmitContext_) {
        return kIOReturnNotReady;
    }
    // Transport starts immediately when requested. A content producer may
    // choose DATA, NO-DATA, empty, or another fully-framed packet form without
    // transport inspecting that choice.
    ASFW_LOG(Isoch, "IsochService: Starting prepared IT");
    const kern_return_t kr = isochTransmitContext_->Start();
    if (kr != kIOReturnSuccess) {
        return kr;
    }
    for (auto& ctx : secondaryTransmitContexts_) {
        if (ctx) {
            const kern_return_t skr = ctx->Start();
            if (skr != kIOReturnSuccess) {
                ASFW_LOG(Isoch, "IsochService: secondary IT start failed: 0x%08x", skr);
                UpdateStreamingActiveState();
                return skr;
            }
        }
    }
    UpdateStreamingActiveState();
    return kIOReturnSuccess;
}

kern_return_t IsochService::StopTransmit() {
    kern_return_t result = kIOReturnSuccess;
    if (isochTransmitContext_) {
        result = isochTransmitContext_->Stop();
    }
    for (auto& ctx : secondaryTransmitContexts_) {
        if (ctx) {
            const kern_return_t stopStatus = ctx->Stop();
            if (stopStatus != kIOReturnSuccess && result == kIOReturnSuccess) {
                result = stopStatus;
            }
        }
    }
    UpdateStreamingActiveState();
    return result;
}

kern_return_t IsochService::StopAll() {
    const kern_return_t receiveStatus = StopReceive();
    const kern_return_t transmitStatus = StopTransmit();
    if (receiveStatus != kIOReturnSuccess || transmitStatus != kIOReturnSuccess) {
        const kern_return_t failure = receiveStatus != kIOReturnSuccess ? receiveStatus : transmitStatus;
        if (hardware_ && hardware_->HardwareGone()) {
            // A revoked provider cannot DMA. This is the inverse of the live-
            // hardware timeout rule above: release rather than retain every
            // software-owned DMA mapping.
            ASFW_LOG(Isoch,
                     "[Lifecycle] IsochService StopAll hardware-gone action=release "
                     "receive=0x%08x transmit=0x%08x",
                     receiveStatus, transmitStatus);
            return kIOReturnSuccess;
        }
        ASFW_LOG_ERROR(Isoch,
                       "IsochService: StopAll did not quiesce every context kr=0x%08x; retaining DMA mappings",
                       failure);
        return failure;
    }
    if (hardware_ && hardware_->HardwareGone()) {
        ASFW_LOG(Isoch,
                 "[Lifecycle] IsochService StopAll hardware-gone action=release receive=0x%08x transmit=0x%08x",
                 receiveStatus, transmitStatus);
    }
    return kIOReturnSuccess;
}

bool IsochService::HardwareGone() const noexcept {
    return hardware_ && hardware_->HardwareGone();
}

void IsochService::SetReceiveConsumer(
    uint32_t streamIndex, ASFW::Isoch::IIsochReceiveConsumer* consumer) noexcept {
    if (streamIndex >= kMaxStreamsPerDirection) {
        return;
    }
    receiveConsumers_[streamIndex] = consumer;
    if (auto* context = ReceiveContext(streamIndex)) {
        context->SetReceiveConsumer(consumer);
    }
}

void IsochService::SetTxPreparationCallback(TxPreparationCallback callback) noexcept {
    txPreparationCallback_ = std::move(callback);
    if (isochTransmitContext_) {
        isochTransmitContext_->SetTxPreparationCallback(txPreparationCallback_);
    }
}

kern_return_t IsochService::AllocateTxIsochResources(uint32_t streamIndex, uint32_t numSlots,
                                                     uint32_t maxPacketBytes,
                                                     uint32_t interruptInterval,
                                                     IOMemoryDescriptor** outPayloadSlab,
                                                     IOMemoryDescriptor** outMetadataRing,
                                                     IOMemoryDescriptor** outControlBlock) {
    if (!outPayloadSlab || !outMetadataRing || !outControlBlock) {
        return kIOReturnBadArgument;
    }
    if (streamIndex >= kMaxStreamsPerDirection) {
        return kIOReturnBadArgument;
    }
    *outPayloadSlab = nullptr;
    *outMetadataRing = nullptr;
    *outControlBlock = nullptr;

    // Free only this stream's prior resources; other streams keep theirs.
    txPayloadSlab_[streamIndex] = nullptr;
    txMetadataRing_[streamIndex] = nullptr;
    txControlBlock_[streamIndex] = nullptr;

    // 1. Allocate payload slab (page-aligned)
    const size_t payloadSlabBytes = static_cast<size_t>(numSlots) * maxPacketBytes;
    IOBufferMemoryDescriptor* payloadDescriptor = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, payloadSlabBytes,
                                                        4096, &payloadDescriptor);
    if (kr != kIOReturnSuccess || !payloadDescriptor) {
        ASFW_LOG(Isoch, "IsochService: Failed to allocate payload slab: 0x%08x", kr);
        return (kr == kIOReturnSuccess) ? kIOReturnNoMemory : kr;
    }
    txPayloadSlab_[streamIndex] = Common::AdoptRetained(payloadDescriptor);

    // 2. Allocate metadata ring (cacheline aligned)
    const size_t metadataRingBytes =
        static_cast<size_t>(numSlots) * sizeof(ASFW::Isoch::IsochTxPacketMeta);
    IOBufferMemoryDescriptor* metadataDescriptor = nullptr;
    kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, metadataRingBytes, 64,
                                          &metadataDescriptor);
    if (kr != kIOReturnSuccess || !metadataDescriptor) {
        ASFW_LOG(Isoch, "IsochService: Failed to allocate metadata ring: 0x%08x", kr);
        txPayloadSlab_[streamIndex] = nullptr;
        return (kr == kIOReturnSuccess) ? kIOReturnNoMemory : kr;
    }
    txMetadataRing_[streamIndex] = Common::AdoptRetained(metadataDescriptor);

    // 3. Allocate control block (cacheline aligned)
    const size_t controlBlockBytes = sizeof(ASFW::Isoch::IsochTxQueueControl);
    IOBufferMemoryDescriptor* controlDescriptor = nullptr;
    kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, controlBlockBytes, 64,
                                          &controlDescriptor);
    if (kr != kIOReturnSuccess || !controlDescriptor) {
        ASFW_LOG(Isoch, "IsochService: Failed to allocate control block: 0x%08x", kr);
        txPayloadSlab_[streamIndex] = nullptr;
        txMetadataRing_[streamIndex] = nullptr;
        return (kr == kIOReturnSuccess) ? kIOReturnNoMemory : kr;
    }
    txControlBlock_[streamIndex] = Common::AdoptRetained(controlDescriptor);

    // Return the descriptors to the caller with retained references
    *outPayloadSlab = txPayloadSlab_[streamIndex].get();
    (*outPayloadSlab)->retain();

    *outMetadataRing = txMetadataRing_[streamIndex].get();
    (*outMetadataRing)->retain();

    *outControlBlock = txControlBlock_[streamIndex].get();
    (*outControlBlock)->retain();

    interruptInterval_ = interruptInterval;

    ASFW_LOG(Isoch, "IsochService: Allocated Tx isoch resources stream %u. numSlots=%u slotSize=%u",
             streamIndex, numSlots, maxPacketBytes);
    return kIOReturnSuccess;
}

kern_return_t IsochService::FreeTxIsochResources() {
    for (uint32_t i = 0; i < kMaxStreamsPerDirection; ++i) {
        txPayloadSlab_[i] = nullptr;
        txMetadataRing_[i] = nullptr;
        txControlBlock_[i] = nullptr;
    }
    ASFW_LOG(Isoch, "IsochService: Freed Tx isoch resources");
    return kIOReturnSuccess;
}

kern_return_t IsochService::GetCycleTimePair(uint64_t* outHostTimeMid, uint32_t* outCycleTimer,
                                             HardwareInterface& hardware) {
    if (!outHostTimeMid || !outCycleTimer) {
        return kIOReturnBadArgument;
    }

    auto access = hardware.TryBeginAccess();
    if (!access) return kIOReturnNotReady;
    const uint32_t cycleTimer = access.Read(static_cast<Register32>(Register32::kCycleTimer));
    const uint64_t hostTime = mach_absolute_time();

    *outHostTimeMid = hostTime;
    *outCycleTimer = cycleTimer;
    return kIOReturnSuccess;
}

void IsochService::UpdateStreamingActiveState() noexcept {
    if (!hardware_) {
        return;
    }
    bool active = false;
    if (isochReceiveContext_ && isochReceiveContext_->GetState() == IRPolicy::State::Running) {
        active = true;
    }
    for (auto& ctx : secondaryReceiveContexts_) {
        if (ctx && ctx->GetState() == IRPolicy::State::Running) {
            active = true;
        }
    }
    if (isochTransmitContext_ && isochTransmitContext_->GetState() == ITState::Running) {
        active = true;
    }
    for (auto& ctx : secondaryTransmitContexts_) {
        if (ctx && ctx->GetState() == ITState::Running) {
            active = true;
        }
    }
    hardware_->SetIsochStreamingActive(active);
}

} // namespace ASFW::Driver
