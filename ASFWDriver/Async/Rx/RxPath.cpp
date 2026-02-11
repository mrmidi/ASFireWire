#include "RxPath.hpp"
#include "ARPacketParser.hpp"
#include "../../Hardware/OHCIEventCodes.hpp"
#include "../../Hardware/OHCIDescriptors.hpp"
#include "../../Hardware/IEEE1394.hpp"
#include "../../Debug/BusResetPacketCapture.hpp"
#include "../../Phy/PhyPackets.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../ResponseCode.hpp"
#include "../../Common/FWCommon.hpp"

#include <DriverKit/IOLib.h>
#include <cstring>

namespace ASFW::Async::Rx {

RxPath::RxPath(ARRequestContext& arReqContext,
               ARResponseContext& arRespContext,
               TrackingActor& tracking,
               ASFW::Async::Bus::GenerationTracker& generationTracker,
               PacketRouter& packetRouter)
    : arRequestContext_(arReqContext)
    , arResponseContext_(arRespContext)
    , tracking_(tracking)
    , generationTracker_(generationTracker)
    , packetRouter_(packetRouter)
{
    packetParser_ = std::make_unique<ARPacketParser>();

    // Route PHY packets (tCode=0xE) in AR Request context through RxPath
    packetRouter_.RegisterRequestHandler(
        HW::AsyncRequestHeader::kTcodePhyPacket,
        [this](const ARPacketView& view) {
            this->HandlePhyRequestPacket(view);
            return ResponseCode::NoResponse;  // PHY packets never generate a response
        });
}

void RxPath::ProcessARInterrupts(std::atomic<uint32_t>& is_bus_reset_in_progress,
                                  bool isRunning,
                                  Debug::BusResetPacketCapture* busResetCapture) {
    const bool inReset = is_bus_reset_in_progress.load(std::memory_order_acquire);

    if (!isRunning) {
        return;
    }

    // Process both contexts in sequence
    // CRITICAL: Keep AR Request alive during bus reset for PHY/bus-reset packets (OHCI §C.3)
    // Only gate AR Response context during reset

    // Process AR Request context (always process, even during reset)
    {
        auto* ctx = &arRequestContext_;
        const char* ctxLabel = "AR Request";
        const ARContextType ctxType = ARContextType::Request;

        auto recycle = [&](size_t descriptorIndex) {
            const kern_return_t recycleKr = ctx->Recycle(descriptorIndex);
            if (recycleKr != kIOReturnSuccess) {
                ASFW_LOG(Async,
                         "RxPath: Failed to recycle descriptor %zu for %{public}s (kr=0x%08x)",
                         descriptorIndex,
                         ctxLabel,
                         recycleKr);
            }
        };

        // Make capture available to PacketRouter handlers (same thread, same call stack)
        currentBusResetCapture_ = busResetCapture;

        uint32_t buffersProcessed = 0;
        while (auto bufferInfo = ctx->Dequeue()) {
            const auto& info = *bufferInfo;
            buffersProcessed++;

            const std::size_t startOffset = info.startOffset;

            ASFW_LOG_HEX(Async, "RxPath AR Request Buffer #%u: vaddr=%p startOffset=%zu size=%zu index=%zu",
                         buffersProcessed,
                         info.virtualAddress,
                         startOffset,
                         info.bytesFilled,
                         info.descriptorIndex);

            if (!info.virtualAddress) {
                ASFW_LOG_HEX(Async, "RxPath AR Request Buffer #%u: NULL virtual address, recycling", buffersProcessed);
                recycle(info.descriptorIndex);
                continue;
            }

            const uint8_t* bufferStart = static_cast<const uint8_t*>(info.virtualAddress);
            const std::size_t bufferSize = info.bytesFilled;

            // If we consumed everything, recycle the descriptor
            // Note: For AR Request, we only recycle if the buffer was completely full or empty
            // to avoid stalling the DMA engine if it's still filling the buffer.
            if (bufferSize == 0 || bufferSize <= startOffset) {
                // TODO: requires hardware testing to confirm if recycling here is safe
                // or if it causes stalls. If AR Request gets stuck, disable this.
                recycle(info.descriptorIndex);
                continue;
            }

            // Optional debug dump stays as-is…
            if (bufferSize >= 32) {
                ASFW_LOG_HEX(Async, "RxPath AR Request Buffer #%u first 128 bytes (showing 32-byte chunks):", buffersProcessed);
                const size_t dumpSize = (bufferSize < 128) ? bufferSize : 128;
                for (size_t i = 0; i < dumpSize; i += 32) {
                    const size_t chunkSize = ((i + 32) <= dumpSize) ? 32 : (dumpSize - i);
                    const uint8_t* chunk = bufferStart + i;

                    if (chunkSize >= 16) {
                        ASFW_LOG_HEX(Async, "  [%04zx] %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                                     i,
                                     chunk[0], chunk[1], chunk[2], chunk[3],
                                     chunk[4], chunk[5], chunk[6], chunk[7],
                                     chunk[8], chunk[9], chunk[10], chunk[11],
                                     chunk[12], chunk[13], chunk[14], chunk[15]);
                        if (chunkSize == 32) {
                            ASFW_LOG_HEX(Async, "  [%04zx] %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                                         i + 16,
                                         chunk[16], chunk[17], chunk[18], chunk[19],
                                         chunk[20], chunk[21], chunk[22], chunk[23],
                                         chunk[24], chunk[25], chunk[26], chunk[27],
                                         chunk[28], chunk[29], chunk[30], chunk[31]);
                        }
                    }
                }
            }

            // Route ONLY the NEW bytes via PacketRouter
            const uint8_t* newDataStart = bufferStart + startOffset;
            const std::size_t newDataSize = bufferSize - startOffset;

            ASFW_LOG_HEX(Async,
                         "RxPath AR Request Buffer #%u: routing %zu NEW bytes from offset %zu",
                         buffersProcessed, newDataSize, startOffset);

            packetRouter_.RoutePacket(
                ARContextType::Request,
                std::span<const uint8_t>(newDataStart, newDataSize));

            // CRITICAL: still do NOT recycle AR Request buffers here.
            // recycle(info.descriptorIndex);  // remains disabled
        }

        ASFW_LOG_V2(Async, "RxPath: Processed %u buffers from %{public}s",
                 buffersProcessed, ctxLabel);
    }

    // Process AR Response context (skip during reset)
    if (!inReset) {
        auto* ctx = &arResponseContext_;
        const char* ctxLabel = "AR Response";
        const ARContextType ctxType = ARContextType::Response;

        // DIAGNOSTIC: Always dump first 64 bytes of AR Response buffer on interrupt
        // This shows raw buffer contents BEFORE cache invalidation and dequeue
        {
            auto& bufferRing = ctx->GetBufferRing();

            // Dump descriptor[0] status (resCount/reqCount) BEFORE cache invalidation
            auto* descBase = static_cast<HW::OHCIDescriptor*>(bufferRing.DescriptorBaseVA());
            if (descBase) {
                const auto& desc = descBase[0];
                const uint16_t resCount = HW::AR_resCount(desc);
                const uint16_t reqCount = static_cast<uint16_t>(desc.control & 0xFFFF);
                const uint16_t xferStatus = HW::AR_xferStatus(desc);
                ASFW_LOG_HEX(Async, "🔍 AR/RSP interrupt: Descriptor[0] BEFORE cache invalidation:");
                ASFW_LOG_HEX(Async, "    statusWord=0x%08X control=0x%08X",
                             desc.statusWord, desc.control);
                ASFW_LOG_HEX(Async, "    resCount=%u reqCount=%u xferStatus=0x%04X %{public}s",
                             resCount, reqCount, xferStatus,
                             (resCount == reqCount) ? "(EMPTY)" : "(FILLED)");
            }

            void* firstBuffer = bufferRing.GetBufferAddress(0);
            if (firstBuffer) {
                const uint8_t* bytes = static_cast<const uint8_t*>(firstBuffer);
                ASFW_LOG_HEX(Async, "🔍 AR/RSP interrupt: Buffer[0] first 64 bytes (RAW, before dequeue):");
                ASFW_LOG_HEX(Async, "  [00] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
                ASFW_LOG_HEX(Async, "  [16] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                             bytes[16], bytes[17], bytes[18], bytes[19], bytes[20], bytes[21], bytes[22], bytes[23],
                             bytes[24], bytes[25], bytes[26], bytes[27], bytes[28], bytes[29], bytes[30], bytes[31]);
                ASFW_LOG_HEX(Async, "  [32] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                             bytes[32], bytes[33], bytes[34], bytes[35], bytes[36], bytes[37], bytes[38], bytes[39],
                             bytes[40], bytes[41], bytes[42], bytes[43], bytes[44], bytes[45], bytes[46], bytes[47]);
                ASFW_LOG_HEX(Async, "  [48] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                             bytes[48], bytes[49], bytes[50], bytes[51], bytes[52], bytes[53], bytes[54], bytes[55],
                             bytes[56], bytes[57], bytes[58], bytes[59], bytes[60], bytes[61], bytes[62], bytes[63]);
            }
        }

        auto recycle = [&](size_t descriptorIndex) {
            const kern_return_t recycleKr = ctx->Recycle(descriptorIndex);
            if (recycleKr != kIOReturnSuccess) {
                ASFW_LOG(Async,
                         "RxPath: Failed to recycle descriptor %zu for %{public}s (kr=0x%08x)",
                         descriptorIndex,
                         ctxLabel,
                         recycleKr);
            }
        };

        uint32_t buffersProcessed = 0;
        uint32_t packetsFound = 0;

        while (auto bufferInfo = ctx->Dequeue()) {
            const auto& info = *bufferInfo;
            buffersProcessed++;

            if (!info.virtualAddress) {
                recycle(info.descriptorIndex);
                continue;
            }

            const uint8_t* bufferStart = static_cast<const uint8_t*>(info.virtualAddress);
            const std::size_t bufferSize = info.bytesFilled;

            // CRITICAL: AR DMA stream semantics - startOffset indicates where NEW packets begin
            // Per OHCI §3.3, §8.4.2: Multiple packets accumulate in same buffer across interrupts.
            // We must parse ONLY from [startOffset, bytesFilled), not re-process old packets.
            const std::size_t startOffset = info.startOffset;

            if (bufferSize == 0 || bufferSize <= startOffset) {
                // No new data in this call
                recycle(info.descriptorIndex);
                continue;
            }

            // Log the NEW packet data (from startOffset onward)
            const uint8_t* newDataStart = bufferStart + startOffset;
            const std::size_t newDataSize = bufferSize - startOffset;

            // V4/HEX: Hexdump AR Response NEW packet data for diagnostics (runtime controlled)
            // CRITICAL: OHCI AR DMA stores each quadlet in little-endian format
            if (newDataSize >= 16) {
                uint32_t q0, q1;
                __builtin_memcpy(&q0, newDataStart, 4);
                __builtin_memcpy(&q1, newDataStart + 4, 4);
                q0 = OSSwapLittleToHostInt32(q0);  // LE to host (no-op on arm64)
                q1 = OSSwapLittleToHostInt32(q1);

                // IEEE 1394 packet format (after LE load):
                // Q0: [destID:16][tLabel:6][rt:2][tCode:4][pri:4]
                // Q1: [srcID:16][rCode:4][offset_high:12]
                const uint8_t tCode_dbg  = static_cast<uint8_t>((q0 >> 4) & 0xF);
                const uint8_t tLabel_dbg = static_cast<uint8_t>((q0 >> 10) & 0x3F);
                const uint8_t rCode_dbg  = static_cast<uint8_t>((q1 >> 12) & 0xF);

                ASFW_LOG_HEX(Async, "AR/RSP NEW data at offset %zu (total=%zu):"
                                    " %02X %02X %02X %02X  %02X %02X %02X %02X"
                                    " %02X %02X %02X %02X  %02X %02X %02X %02X",
                             startOffset, bufferSize,
                             newDataStart[0], newDataStart[1], newDataStart[2], newDataStart[3],
                             newDataStart[4], newDataStart[5], newDataStart[6], newDataStart[7],
                             newDataStart[8], newDataStart[9], newDataStart[10], newDataStart[11],
                             newDataStart[12], newDataStart[13], newDataStart[14], newDataStart[15]);

                ASFW_LOG_HEX(Async, "AR/RSP NEW q0=0x%08X q1=0x%08X  → tCode=0x%X, tLabel=%u, rCode=0x%X",
                             q0, q1, tCode_dbg, tLabel_dbg, rCode_dbg);
            }

            // Per OHCI §8.4.2: Buffer may contain MULTIPLE packets
            // Parse ONLY the NEW packets from [startOffset, bytesFilled)
            std::size_t offset = startOffset;

            while (offset < bufferSize) {
                // Phase 2.2: ARPacketParser::ParseNext now takes std::span
                auto packetInfo = ARPacketParser::ParseNext(std::span<const uint8_t>(bufferStart, bufferSize), offset);

                if (!packetInfo.has_value()) {
                    break;
                }

                packetsFound++;

                // Process this packet (pass PacketInfo directly)
                ProcessReceivedPacket(ctxType, *packetInfo, busResetCapture);

                offset += packetInfo->totalLength;
            }

            // CRITICAL: Do NOT recycle the buffer after processing packets!
            //
            // Per OHCI §3.3, §8.4.2 bufferFill mode:
            // - Hardware ACCUMULATES packets in the same buffer until nearly full
            // - Hardware raises interrupt after EACH packet (not after buffer fills)
            // - Software should process packets incrementally WITHOUT recycling
            // - Hardware advances to next descriptor when current buffer exhausted (resCount≈0)
            // - Software should recycle old buffer ONLY when hardware has moved to next
            //
            // THE BUG: Calling recycle() here resets resCount=reqCount, making buffer "empty" again.
            // Hardware sees "empty" buffer and writes next packet to SAME buffer again!
            // Result: buffer[0] never fills, hardware never advances, packets keep appending.
            //
            // THE FIX: Do NOT call recycle() here. Let hardware fill the buffer completely.
            // When buffer is exhausted, hardware will automatically advance to next descriptor.
            // Future: Implement proper recycling when xferStatus.active==1 on next descriptor.
            //
            // recycle(info.descriptorIndex);  // ⚠️  DISABLED: Causes buffer to never fill!

            ASFW_LOG_V2(Async,
                     "✅ RxPath AR/RSP: Processed %zu NEW bytes from buffer[%zu] "
                     "(offset %zu→%zu, total=%zu) - buffer NOT recycled, letting HW fill",
                     newDataSize, info.descriptorIndex, startOffset, offset, bufferSize);
        }

        ASFW_LOG_V2(Async, "RxPath: Processed %u packets in %u buffers from %{public}s",
                 packetsFound, buffersProcessed, ctxLabel);

        // DIAGNOSTIC: If no packets processed despite interrupt, dump first 64 bytes of buffer
        // This helps diagnose cache coherency issues or hardware problems
        if (buffersProcessed == 0 && packetsFound == 0) {
            ASFW_LOG_V3(Async, "AR Response: No packets read for this interrupt");

            // Get buffer ring from context
            auto& bufferRing = ctx->GetBufferRing();
            void* firstBuffer = bufferRing.GetBufferAddress(0);

            if (firstBuffer) {
                // Dump first 64 bytes for diagnostics
                const uint8_t* bytes = static_cast<const uint8_t*>(firstBuffer);
                ASFW_LOG_HEX(Async, "AR Response Buffer[0] first 64 bytes:");
                ASFW_LOG_HEX(Async, "  [00] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
                ASFW_LOG_HEX(Async, "  [16] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                             bytes[16], bytes[17], bytes[18], bytes[19], bytes[20], bytes[21], bytes[22], bytes[23],
                             bytes[24], bytes[25], bytes[26], bytes[27], bytes[28], bytes[29], bytes[30], bytes[31]);
                ASFW_LOG_HEX(Async, "  [32] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                             bytes[32], bytes[33], bytes[34], bytes[35], bytes[36], bytes[37], bytes[38], bytes[39],
                             bytes[40], bytes[41], bytes[42], bytes[43], bytes[44], bytes[45], bytes[46], bytes[47]);
                ASFW_LOG_HEX(Async, "  [48] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                             bytes[48], bytes[49], bytes[50], bytes[51], bytes[52], bytes[53], bytes[54], bytes[55],
                             bytes[56], bytes[57], bytes[58], bytes[59], bytes[60], bytes[61], bytes[62], bytes[63]);
            } else {
                ASFW_LOG_HEX(Async, "⚠️  AR Response: Cannot get buffer address for dump");
            }
        }
    } else {
        ASFW_LOG(Async, "RxPath: Skipping AR Response during bus reset");
    }

    // Clear capture pointer after this interrupt batch
    currentBusResetCapture_ = nullptr;
}

void RxPath::ProcessReceivedPacket(ARContextType contextType,
                                   const ARPacketParser::PacketInfo& info,
                                   Debug::BusResetPacketCapture* busResetCapture) {
    // Use PacketInfo fields directly - parser already extracted and validated everything
    const uint8_t tCode = info.tCode;
    const uint8_t rCode = info.rCode;
    const uint16_t xferStatus = static_cast<uint16_t>(info.xferStatus & 0xFFFF);
    const OHCIEventCode eventCode = static_cast<OHCIEventCode>(xferStatus & 0x1F);

    if (contextType == ARContextType::Request) {
        ASFW_LOG(Async,
                 "RxPath::ProcessReceivedPacket called with AR Request context – should not happen");
        return;
    }

    // AR Response context: Handle response packets
    if (tCode == HW::AsyncRequestHeader::kTcodePhyPacket) {
        if (eventCode == OHCIEventCode::kEvtBusReset) {
            ASFW_LOG(Async,
                   "RxPath: Synthesised bus reset marker observed in AR Response stream");
            // Note: OnBusReset() is handled at a higher level by AsyncSubsystem
        }
        return;
    }

    // Extract tLabel, sourceID, destinationID from IEEE 1394 packet header
    // CRITICAL: info.packetStart points at LE DMA buffer. Load as LE quadlets.
    // The ARPacketParser already validated tCode/rCode - we just need node IDs and tLabel.
    uint32_t q0_le, q1_le;
    __builtin_memcpy(&q0_le, info.packetStart, 4);
    __builtin_memcpy(&q1_le, info.packetStart + 4, 4);
    
    // Convert LE quadlets to host order (no-op on ARM64, but documents intent)
    const uint32_t q0 = OSSwapLittleToHostInt32(q0_le);
    const uint32_t q1 = OSSwapLittleToHostInt32(q1_le);

    // Extract fields directly from host-order quadlets
    // IEEE 1394-2008 §6.2.2.1 (matches Linux packet-header-definitions.h):
    // Q0: [destination_ID:16][tLabel:6][rt:2][tCode:4][pri:4]
    // Q1: [source_ID:16][rCode:4][reserved/offset_high:12]
    const uint16_t destinationID = static_cast<uint16_t>(q0 >> 16);
    const uint8_t tLabel = static_cast<uint8_t>((q0 >> 10) & 0x3F);
    const uint16_t sourceID = static_cast<uint16_t>(q1 >> 16);

    const auto busState = generationTracker_.GetCurrentState();
    const uint16_t currentGen = busState.generation16;

    ASFW_LOG_V3(Async, "🔍 RxPath AR response: tCode=0x%X rCode=0x%X tLabel=%u generation=%u srcID=0x%04X dstID=0x%04X - attempting match",
             tCode, rCode, tLabel, currentGen, sourceID, destinationID);

    // Create RxResponse struct using PacketInfo fields directly
    RxResponse rxResponse{};
    rxResponse.generation = currentGen;
    rxResponse.sourceNodeID = sourceID;
    rxResponse.destinationNodeID = destinationID;
    rxResponse.tLabel = tLabel;
    rxResponse.tCode = tCode;
    rxResponse.rCode = rCode;
    rxResponse.eventCode = eventCode;
    rxResponse.hardwareTimeStamp = static_cast<uint16_t>(info.timeStamp);

    // Extract payload using PacketInfo fields
    const uint8_t* payloadPtr = info.packetStart + info.headerLength;
    size_t payloadLen = info.dataLength;

    // Special-case: Read Quadlet Response (0x6) — data lives in header q3
    if (info.tCode == 0x6) {  // kTCodeReadQuadletResponse
        payloadPtr = info.packetStart + 12; // q3 (offset 12-15)
        payloadLen = 4;
    }

    // CRITICAL: DMA buffers are mapped as device memory (kIOMemoryMapCacheModeInhibit).
    // On ARM64, device memory requires strict natural alignment for all accesses.
    // std::memcpy downstream may use ldr x (8-byte load) which requires 8-byte alignment,
    // but packet payload offsets within AR buffers are only guaranteed 4-byte aligned
    // (packets are quadlet-aligned). This causes EXC_ARM_DA_ALIGN / SIGBUS for len >= 8.
    // Fix: copy payload into stack buffer using quadlet-aligned reads before passing downstream.
    static constexpr size_t kMaxARPayloadBytes = ASFW::FW::MaxPayload::kS800;  // 4096
    if (payloadLen > kMaxARPayloadBytes) {
        ASFW_LOG(Async, "⚠️ AR/RSP: payload %zu exceeds max %zu — dropping packet",
                 payloadLen, kMaxARPayloadBytes);
        return;
    }
    alignas(4) uint8_t payloadCopy[kMaxARPayloadBytes];
    {
        size_t i = 0;
        // Quadlet-aligned copy (OHCI packets are always quadlet-aligned)
        for (; i + 4 <= payloadLen; i += 4) {
            uint32_t tmp;
            __builtin_memcpy(&tmp, payloadPtr + i, 4);
            __builtin_memcpy(payloadCopy + i, &tmp, 4);
        }
        // Remaining tail bytes
        for (; i < payloadLen; ++i) {
            payloadCopy[i] = payloadPtr[i];
        }
    }

    // NOTE: span points to stack-local payloadCopy — valid only for this synchronous call chain.
    rxResponse.payload = std::span<const uint8_t>(payloadCopy, payloadLen);

    // V2: Compact AR response one-liner for packet flow visibility
    ASFW_LOG_V2(Async, "📥 AR/RSP: tCode=0x%X rCode=0x%X tLabel=%u src=0x%04X→dst=0x%04X payload=%zu bytes",
               tCode, rCode, tLabel, sourceID, destinationID, payloadLen);

    // Delegate to Tracking actor
    tracking_.OnRxResponse(rxResponse);
}

void RxPath::HandleSyntheticBusResetPacket(const uint32_t* quadlets, uint8_t newGeneration, Debug::BusResetPacketCapture* busResetCapture) {
    // OHCI §8.4.2.3, Linux handle_ar_packet() Bus-Reset path
    // This function is called when we detect the synthetic Bus-Reset packet
    // Format per OHCI Table 8-4:
    //   q0: tcode=0xE, reserved fields (big-endian)
    //   q1: selfIDGeneration[23:16], event=0x5h09[15:0] (big-endian)

    if (!quadlets) {
        ASFW_LOG(Async, "RxPath::HandleSyntheticBusResetPacket: NULL quadlets pointer");
        return;
    }

    // Log raw packet data for verification
    // CRITICAL: OHCI DMA is LITTLE-ENDIAN! Must swap to get wire format.
    // Linux: cond_le32_to_cpu() - we use OSSwapLittleToHostInt32() to swap LE→BE
    const uint32_t q0 = OSSwapLittleToHostInt32(quadlets[0]);  // LE bytes → BE wire format
    const uint32_t q1 = OSSwapLittleToHostInt32(quadlets[1]);  // LE bytes → BE wire format

    // Extract tCode from first byte (high byte in big-endian wire format)
    const uint8_t wireByte0 = static_cast<uint8_t>(q0 >> 24);
    const uint8_t tCode = (wireByte0 >> 4) & 0xF;

    const uint8_t genFromPacket = static_cast<uint8_t>((q1 >> 16) & 0xFF);

    ASFW_LOG_HEX(Async, "RxPath Bus-Reset packet parsing:");
    ASFW_LOG_HEX(Async, "  q0 (host): 0x%08X wireByte0=0x%02X", q0, wireByte0);
    ASFW_LOG_HEX(Async, "  q1 (host): 0x%08X", q1);
    ASFW_LOG_HEX(Async, "  tCode: 0x%X (should be 0xE)", tCode);
    ASFW_LOG_HEX(Async, "  generation from packet: %u (arg: %u)", genFromPacket, newGeneration);

    ASFW_LOG(Async, "RxPath: Synthetic bus reset packet: tCode=0x%X gen=%u (controller=%u)",
             tCode, genFromPacket, newGeneration);

    if (genFromPacket != newGeneration) {
        ASFW_LOG(Async, "⚠️  WARNING: Generation mismatch in bus-reset packet! (%u vs %u)",
                 genFromPacket, newGeneration);
    }

    // Capture packet for debugging/GUI
    if (busResetCapture) {
        char context[64];
        std::snprintf(context, sizeof(context), "RxPath Synthetic packet, gen %u (informational)", newGeneration);
        busResetCapture->CapturePacket(quadlets, newGeneration, context);
        ASFW_LOG(Async, "RxPath: Bus reset packet captured (total: %zu), packet gen=%u (informational only)",
                 busResetCapture->GetCount(), newGeneration);
    }

    // NOTE: Do NOT update generation tracker from synthetic bus reset packet!
    // The packet generation is just AR buffer metadata and may be stale.
    // The AUTHORITATIVE generation comes from the SelfIDCount register (OHCI §11.2)
    // and is set via ConfirmBusGeneration() after Self-ID decode completes.
    // This prevents race where synthetic packet overwrites the real generation.
    //
    // generationTracker_.OnSyntheticBusReset(newGeneration);  // REMOVED - causes race!
}

void RxPath::HandlePhyRequestPacket(const ARPacketView& view) {
    // Decode event code from OHCI trailer’s xferStatus low bits
    const uint16_t xferStatus = view.xferStatus;
    const OHCIEventCode eventCode = static_cast<OHCIEventCode>(xferStatus & 0x1F);

    // Need at least q0 + q1 in header
    if (view.header.size() < 8) {
        ASFW_LOG(Async,
                 "RxPath AR/RQ PHY handler: short header (len=%zu), event=0x%02X",
                 view.header.size(), static_cast<unsigned>(eventCode));
        return;
    }

    uint32_t q0_le = 0;
    uint32_t q1_le = 0;
    __builtin_memcpy(&q0_le, view.header.data(), 4);
    __builtin_memcpy(&q1_le, view.header.data() + 4, 4);

    const uint32_t q0 = OSSwapLittleToHostInt32(q0_le);
    const uint32_t q1 = OSSwapLittleToHostInt32(q1_le);

    constexpr OHCIEventCode OHCI_EVT_BUS_RESET = OHCIEventCode::kEvtBusReset;

    if (eventCode == OHCI_EVT_BUS_RESET) {
        // Extract generation from packet (OHCI Table 8-4)
        const uint8_t genFromPacket = static_cast<uint8_t>((q1 >> 16) & 0xFF);

        ASFW_LOG(Async,
                 "🔥 SYNTHETIC BUS-RESET PACKET via PacketRouter: gen=%u event=0x%02X xferStatus=0x%04X",
                 genFromPacket,
                 static_cast<unsigned>(eventCode),
                 xferStatus);

        if (currentBusResetCapture_) {
            // Reuse existing handler – header holds q0/q1 quadlets
            const uint32_t* quadlets_raw =
                reinterpret_cast<const uint32_t*>(view.header.data());
            HandleSyntheticBusResetPacket(quadlets_raw,
                                          genFromPacket,
                                          currentBusResetCapture_);
        }

        return;
    }

    // Non-reset PHY packets (e.g. alpha PHY config)
    const bool isAlphaConfig = ASFW::Driver::AlphaPhyConfig::IsConfigQuadletHostOrder(q0);
    if (isAlphaConfig) {
        const auto cfg = ASFW::Driver::AlphaPhyConfig::DecodeHostOrder(q0);
        ASFW_LOG(Async,
                 "RxPath AR/RQ: PHY CONFIG (non-reset): rootId=%u R=%d T=%d gap=%u event=0x%02X q0=0x%08x q1=0x%08x",
                 cfg.rootId,
                 cfg.forceRoot ? 1 : 0,
                 cfg.gapCountOptimization ? 1 : 0,
                 cfg.gapCount,
                 static_cast<unsigned>(eventCode),
                 q0,
                 q1);
    } else {
        ASFW_LOG(Async,
                 "RxPath AR/RQ: PHY packet (non-reset): event=0x%02X q0=0x%08x q1=0x%08x len=%zu",
                 static_cast<unsigned>(eventCode),
                 q0,
                 q1,
                 view.header.size());
    }
}

} // namespace ASFW::Async::Rx
