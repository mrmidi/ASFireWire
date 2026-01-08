#pragma once

// AsyncCommandImpl.hpp - Template implementation for AsyncCommand<Derived>::Submit()
// Must be included at end of AsyncCommand.hpp (templates require header-only definition)

#include "../AsyncSubsystem.hpp"
#include "../Track/Tracking.hpp"
#include "../Tx/PacketBuilder.hpp"
#include "../Tx/DescriptorBuilder.hpp"
#include "../Tx/Submitter.hpp"
#include "../../Bus/GenerationTracker.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Async {

template<typename Derived>
AsyncHandle AsyncCommand<Derived>::Submit(AsyncSubsystem& subsys) {
    // Step 1: Prepare transaction context (bus state validation)
    auto txCtxOpt = subsys.PrepareTransactionContext();
    if (!txCtxOpt.has_value()) {
        ASFW_LOG_ERROR(Async, "Command submit failed: PrepareTransactionContext returned nullopt");
        return AsyncHandle{0};
    }
    const TransactionContext& txCtx = txCtxOpt.value();
    
    // Step 2: Build transaction metadata (CRTP dispatch to derived class)
    TxMetadata meta = static_cast<Derived*>(this)->BuildMetadata(txCtx);

    // Normalize destination NodeID: ensure bus number bits (15:6) match local bus
    // hardware expects full 16-bit NodeID for tracking/matching; ROMScanner passes
    // only the 6-bit node value. Pull bus bits from sourceNodeID when absent.
    constexpr uint16_t kNodeMask = 0x003F;
    constexpr uint16_t kBusMask  = 0xFFC0;
    if ((meta.destinationNodeID & kBusMask) == 0) {
        const uint16_t sourceBusBits = static_cast<uint16_t>(txCtx.sourceNodeID & kBusMask);
        meta.destinationNodeID = static_cast<uint16_t>(sourceBusBits | (meta.destinationNodeID & kNodeMask));
    }

    meta.callback = callback_;

    ASFW_LOG_V3(Async, "🔍 [AsyncCommand] Submitting with callback=%p (valid=%d)",
             &callback_, callback_ ? 1 : 0);

    // Step 3: Register transaction with Tracking actor
    AsyncHandle handle = subsys.GetTracking()->RegisterTx(meta);
    if (handle.value == 0) {
        ASFW_LOG_ERROR(Async, "Command submit failed: RegisterTx returned invalid handle");
        return AsyncHandle{0};
    }
    
    // Step 4: Extract transaction label from handle
    auto labelOpt = subsys.GetTracking()->GetLabelFromHandle(handle);
    if (!labelOpt.has_value()) {
        ASFW_LOG_ERROR(Async, "Command submit failed: GetLabelFromHandle returned nullopt for handle=0x%x", 
                       handle.value);
        return AsyncHandle{0};
    }
    const uint8_t label = labelOpt.value();
    
    // Step 5: Build IEEE 1394 packet header (CRTP dispatch)
    auto* packetBuilder = subsys.GetPacketBuilder();
    if (packetBuilder == nullptr) {
        ASFW_LOG_ERROR(Async, "Command submit failed: PacketBuilder unavailable");
        return AsyncHandle{0};
    }

    uint8_t headerBuffer[20]{};  // Max header size (block write: 16 bytes + alignment)
    const size_t headerSize = static_cast<Derived*>(this)->BuildHeader(
        label, txCtx.packetContext, *packetBuilder, headerBuffer);
    if (headerSize == 0) {
        ASFW_LOG_ERROR(Async, "Command submit failed: BuildHeader returned 0 for handle=0x%x", 
                       handle.value);
        return AsyncHandle{0};
    }
    
    // Step 6: Prepare DMA payload (if needed) - CRTP dispatch
    std::unique_ptr<PayloadContext> payload = 
        static_cast<Derived*>(this)->PreparePayload(*subsys.GetHardware());
    const uint64_t payloadIOVA = payload ? payload->DeviceAddress() : 0;
    const uint32_t payloadLen = payload ? static_cast<uint32_t>(payload->Length()) : 0;
    
    // Step 7: Build OHCI descriptor chain (always interrupts on LAST per OHCI spec)
    // Apple's hybrid pattern (IDA @ 0xEDCA lines 207-209, @ 0xDBBE lines 89-129):
    // needsFlush = true for block operations with scatter/gather DMA (complex)
    // needsFlush = false for quadlet operations without DMA (simple)
    const bool needsFlush = (payloadLen > 4);  // Block ops need flush, quadlet ops don't
    
    auto chain = subsys.GetDescriptorBuilder()->BuildTransactionChain(
        headerBuffer, headerSize, payloadIOVA, payloadLen, needsFlush);
    if (!chain.first) {
        ASFW_LOG_ERROR(Async, "Command submit failed: BuildTransactionChain returned null for handle=0x%x",
                       handle.value);
        return AsyncHandle{0};
    }
    
    // Step 8: Tag descriptor with handle for completion matching
    // Use DescriptorBuilder::TagSoftware() to properly tag and sync the descriptor
    subsys.GetDescriptorBuilder()->TagSoftware(chain.last, handle.value);
    
    // Step 9: Submit descriptor chain to AT context
    auto* atReqCtx = subsys.ResolveAtRequestContext();
    if (!atReqCtx) {
        ASFW_LOG_ERROR(Async, "Command submit failed: AT Request context not available");
        return AsyncHandle{0};
    }
    
    auto submitRes = subsys.GetSubmitter()->submit_tx_chain(atReqCtx, std::move(chain));
    if (submitRes.kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Async, "Command submit failed: submit_tx_chain returned kr=0x%x for handle=0x%x",
                       submitRes.kr, handle.value);
        return AsyncHandle{0};
    }
    
    // Step 10: Schedule timeout
    const uint64_t now = subsys.GetCurrentTimeUsec();
    constexpr uint64_t kDefaultTimeoutUsec = 1'000'000;  // 1000ms (relaxed from 200ms)
    subsys.GetTracking()->OnTxPosted(handle, now, kDefaultTimeoutUsec);
    
    // Step 11: Attach payload to PayloadRegistry (if non-null)
    // Convert unique_ptr to shared_ptr before attaching to registry (consumes unique_ptr)
    if (payload) {
        auto payloadShared = PayloadContext::IntoShared(std::move(payload));
        subsys.GetTracking()->Payloads()->Attach(
            handle.value, payloadShared, txCtx.generation);
    }
    
    return handle;
}

} // namespace ASFW::Async
