#include "RxPath.hpp"
#include "ARPacketParser.hpp"
#include "../OHCIEventCodes.hpp"
#include "../OHCI_HW_Specs.hpp"
#include "../../Debug/BusResetPacketCapture.hpp"

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

        uint32_t buffersProcessed = 0;
        while (auto bufferInfo = ctx->Dequeue()) {
            const auto& info = *bufferInfo;
            buffersProcessed++;

            // CRITICAL: AR DMA stream semantics - startOffset indicates where NEW packets begin
            const std::size_t startOffset = info.startOffset;

            ASFW_LOG_BUS_RESET_PACKET("RxPath AR Request Buffer #%u: vaddr=%p startOffset=%zu size=%zu index=%zu",
                                      buffersProcessed,
                                      info.virtualAddress,
                                      startOffset,
                                      info.bytesFilled,
                                      info.descriptorIndex);

            if (!info.virtualAddress) {
                ASFW_LOG_BUS_RESET_PACKET("RxPath AR Request Buffer #%u: NULL virtual address, recycling", buffersProcessed);
                recycle(info.descriptorIndex);
                continue;
            }

            const uint8_t* bufferStart = static_cast<const uint8_t*>(info.virtualAddress);
            const std::size_t bufferSize = info.bytesFilled;

            if (bufferSize == 0 || bufferSize <= startOffset) {
                // No new data
                recycle(info.descriptorIndex);
                continue;
            }

#if ASFW_DEBUG_BUS_RESET_PACKET
            if (bufferSize >= 32) {
                ASFW_LOG_BUS_RESET_PACKET("RxPath AR Request Buffer #%u first 128 bytes (showing 32-byte chunks):", buffersProcessed);
                const size_t dumpSize = (bufferSize < 128) ? bufferSize : 128;
                for (size_t i = 0; i < dumpSize; i += 32) {
                    const size_t chunkSize = ((i + 32) <= dumpSize) ? 32 : (dumpSize - i);
                    const uint8_t* chunk = bufferStart + i;

                    if (chunkSize >= 16) {
                        ASFW_LOG_BUS_RESET_PACKET("  [%04zx] %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                               i,
                               chunk[0], chunk[1], chunk[2], chunk[3],
                               chunk[4], chunk[5], chunk[6], chunk[7],
                               chunk[8], chunk[9], chunk[10], chunk[11],
                               chunk[12], chunk[13], chunk[14], chunk[15]);
                        if (chunkSize == 32) {
                            ASFW_LOG_BUS_RESET_PACKET("  [%04zx] %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                                   i + 16,
                                   chunk[16], chunk[17], chunk[18], chunk[19],
                                   chunk[20], chunk[21], chunk[22], chunk[23],
                                   chunk[24], chunk[25], chunk[26], chunk[27],
                                   chunk[28], chunk[29], chunk[30], chunk[31]);
                        }
                    }
                }
            }
#endif

            // Per OHCI §8.4.2: Buffer may contain MULTIPLE packets
            // Parse buffer as stream, extracting packets one-by-one
            // Parse ONLY the NEW packets from [startOffset, bytesFilled)
            // Source: Linux drivers/firewire/ohci.c:1656-1720 handle_ar_packet()
            std::size_t offset = startOffset;
            uint32_t packetsFound = 0;

            while (offset < bufferSize) {
                // Phase 2.2: ARPacketParser::ParseNext now takes std::span
                auto packetInfo = ARPacketParser::ParseNext(std::span<const uint8_t>(bufferStart, bufferSize), offset);

                if (!packetInfo.has_value()) {
#if ASFW_DEBUG_BUS_RESET_PACKET
                    if (offset < bufferSize) {
                        const size_t remaining = bufferSize - offset;
                        ASFW_LOG_BUS_RESET_PACKET(
                            "RxPath AR buffer exhausted: %zu bytes remaining (incomplete packet or padding)",
                            remaining);
                    }
#endif
                    break;
                }

                packetsFound++;

                // Process this packet (pass PacketInfo directly)
                ProcessReceivedPacket(ctxType, *packetInfo, busResetCapture);

                // Advance to next packet
                offset += packetInfo->totalLength;
            }

            ASFW_LOG_BUS_RESET_PACKET("RxPath AR Request Buffer #%u: Extracted %u NEW packets from offset %zu→%zu (total %zu bytes)",
                                      buffersProcessed, packetsFound, startOffset, offset, bufferSize);

            // CRITICAL: Do NOT recycle AR Request buffers prematurely!
            // Same stream semantics as AR Response - let hardware fill buffer completely.
            // recycle(info.descriptorIndex);  // ⚠️  DISABLED: Causes buffer to never fill!
        }

        ASFW_LOG(Async, "RxPath: Processed %u buffers from %{public}s",
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
                ASFW_LOG(Async, "🔍 AR/RSP interrupt: Descriptor[0] BEFORE cache invalidation:");
                ASFW_LOG(Async, "    statusWord=0x%08X control=0x%08X",
                    desc.statusWord, desc.control);
                ASFW_LOG(Async, "    resCount=%u reqCount=%u xferStatus=0x%04X %{public}s",
                    resCount, reqCount, xferStatus,
                    (resCount == reqCount) ? "(EMPTY)" : "(FILLED)");
            }

            void* firstBuffer = bufferRing.GetBufferAddress(0);
            if (firstBuffer) {
                const uint8_t* bytes = static_cast<const uint8_t*>(firstBuffer);
                ASFW_LOG(Async, "🔍 AR/RSP interrupt: Buffer[0] first 64 bytes (RAW, before dequeue):");
                ASFW_LOG(Async, "  [00] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                    bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
                ASFW_LOG(Async, "  [16] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                    bytes[16], bytes[17], bytes[18], bytes[19], bytes[20], bytes[21], bytes[22], bytes[23],
                    bytes[24], bytes[25], bytes[26], bytes[27], bytes[28], bytes[29], bytes[30], bytes[31]);
                ASFW_LOG(Async, "  [32] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                    bytes[32], bytes[33], bytes[34], bytes[35], bytes[36], bytes[37], bytes[38], bytes[39],
                    bytes[40], bytes[41], bytes[42], bytes[43], bytes[44], bytes[45], bytes[46], bytes[47]);
                ASFW_LOG(Async, "  [48] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
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

#if 1
            // Hexdump AR Response NEW packet data for diagnostics
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

                ASFW_LOG(Async, "AR/RSP NEW data at offset %zu (total=%zu):"
                                " %02X %02X %02X %02X  %02X %02X %02X %02X"
                                " %02X %02X %02X %02X  %02X %02X %02X %02X",
                    startOffset, bufferSize,
                    newDataStart[0], newDataStart[1], newDataStart[2], newDataStart[3],
                    newDataStart[4], newDataStart[5], newDataStart[6], newDataStart[7],
                    newDataStart[8], newDataStart[9], newDataStart[10], newDataStart[11],
                    newDataStart[12], newDataStart[13], newDataStart[14], newDataStart[15]);

                ASFW_LOG(Async, "AR/RSP NEW q0=0x%08X q1=0x%08X  → tCode=0x%X, tLabel=%u, rCode=0x%X",
                    q0, q1, tCode_dbg, tLabel_dbg, rCode_dbg);
            }
#endif

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

            ASFW_LOG(Async,
                     "✅ RxPath AR/RSP: Processed %zu NEW bytes from buffer[%zu] "
                     "(offset %zu→%zu, total=%zu) - buffer NOT recycled, letting HW fill",
                     newDataSize, info.descriptorIndex, startOffset, offset, bufferSize);
        }

        ASFW_LOG(Async, "RxPath: Processed %u packets in %u buffers from %{public}s",
                 packetsFound, buffersProcessed, ctxLabel);

        // DIAGNOSTIC: If no packets processed despite interrupt, dump first 64 bytes of buffer
        // This helps diagnose cache coherency issues or hardware problems
        if (buffersProcessed == 0 && packetsFound == 0) {
            ASFW_LOG(Async, "⚠️  AR Response: No packets read despite interrupt! Dumping first buffer...");

            // Get buffer ring from context
            auto& bufferRing = ctx->GetBufferRing();
            void* firstBuffer = bufferRing.GetBufferAddress(0);

            if (firstBuffer) {
                // Dump first 64 bytes for diagnostics
                const uint8_t* bytes = static_cast<const uint8_t*>(firstBuffer);
                ASFW_LOG(Async, "AR Response Buffer[0] first 64 bytes:");
                ASFW_LOG(Async, "  [00] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
                    bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
                ASFW_LOG(Async, "  [16] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                    bytes[16], bytes[17], bytes[18], bytes[19], bytes[20], bytes[21], bytes[22], bytes[23],
                    bytes[24], bytes[25], bytes[26], bytes[27], bytes[28], bytes[29], bytes[30], bytes[31]);
                ASFW_LOG(Async, "  [32] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                    bytes[32], bytes[33], bytes[34], bytes[35], bytes[36], bytes[37], bytes[38], bytes[39],
                    bytes[40], bytes[41], bytes[42], bytes[43], bytes[44], bytes[45], bytes[46], bytes[47]);
                ASFW_LOG(Async, "  [48] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X",
                    bytes[48], bytes[49], bytes[50], bytes[51], bytes[52], bytes[53], bytes[54], bytes[55],
                    bytes[56], bytes[57], bytes[58], bytes[59], bytes[60], bytes[61], bytes[62], bytes[63]);
            } else {
                ASFW_LOG(Async, "⚠️  AR Response: Cannot get buffer address for dump");
            }
        }
    } else {
        ASFW_LOG(Async, "RxPath: Skipping AR Response during bus reset");
    }
}

void RxPath::ProcessReceivedPacket(ARContextType contextType,
                                   const ARPacketParser::PacketInfo& info,
                                   Debug::BusResetPacketCapture* busResetCapture) {
    // Use PacketInfo fields directly - parser already extracted and validated everything
    const uint8_t tCode = info.tCode;
    const uint8_t rCode = info.rCode;
    const uint16_t xferStatus = static_cast<uint16_t>(info.xferStatus & 0xFFFF);
    const OHCIEventCode eventCode = static_cast<OHCIEventCode>(xferStatus & 0x1F);

    // AR Request context: Handle PHY packets (including synthetic Bus-Reset packet)
    // OHCI §8.4.2.3, §8.5: Controller injects Bus-Reset packet when LinkControl.rcvPhyPkt=1
    if (contextType == ARContextType::Request) {
        // PHY packet (tCode=0xE): Check for Bus-Reset event
        // CRITICAL: Event code comes from TRAILER xferStatus[4:0], NOT from packet body!
        if (tCode == HW::AsyncRequestHeader::kTcodePhyPacket && info.totalLength >= 16) {
            // Load quadlets from LE DMA buffer
            uint32_t q0_le, q1_le;
            __builtin_memcpy(&q0_le, info.packetStart, 4);
            __builtin_memcpy(&q1_le, info.packetStart + 4, 4);
            
            // Convert to host order (no-op on ARM64, but explicit for clarity)
            const uint32_t q0 = OSSwapLittleToHostInt32(q0_le);
            const uint32_t q1 = OSSwapLittleToHostInt32(q1_le);

            // Bus-Reset event code (OHCI §3.1.1 Table 3-2)
            constexpr OHCIEventCode OHCI_EVT_BUS_RESET = OHCIEventCode::kEvtBusReset;

            if (eventCode == OHCI_EVT_BUS_RESET) {
                // Extract selfIDGeneration from quadlet 1 bits[23:16] (OHCI Table 8-4)
                const uint8_t newGeneration = static_cast<uint8_t>((q1 >> 16) & 0xFF);

                ASFW_LOG(Async, "🔥 SYNTHETIC BUS-RESET PACKET: gen=%u event=0x%02X xferStatus=0x%04X",
                         newGeneration, eventCode, xferStatus);

                // Pass raw LE quadlets pointer for HandleSyntheticBusResetPacket
                const uint32_t* quadlets_raw = reinterpret_cast<const uint32_t*>(info.packetStart);
                HandleSyntheticBusResetPacket(quadlets_raw, newGeneration, busResetCapture);
                return;  // Bus-Reset packet fully processed
            }

            // Other PHY packets (not Bus-Reset)
            ASFW_LOG(Async,
                   "RxPath AR/RQ: PHY packet (event=0x%02X) - not Bus-Reset, ignoring", eventCode);
            return;
        }

        // Non-PHY async request packets
        ASFW_LOG(Async,
               "RxPath AR/RQ: Async request packet (tCode=0x%X, event=%{public}s) - ignoring (not yet implemented)",
               tCode, ToString(eventCode));
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

    ASFW_LOG(Async, "🔍 RxPath AR response: tCode=0x%X rCode=0x%X tLabel=%u generation=%u srcID=0x%04X dstID=0x%04X - attempting match",
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

    rxResponse.payload = std::span<const uint8_t>(payloadPtr, payloadLen);

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

    ASFW_LOG_BUS_RESET_PACKET("RxPath Bus-Reset packet parsing:");
    ASFW_LOG_BUS_RESET_PACKET("  q0 (host): 0x%08X wireByte0=0x%02X", q0, wireByte0);
    ASFW_LOG_BUS_RESET_PACKET("  q1 (host): 0x%08X", q1);
    ASFW_LOG_BUS_RESET_PACKET("  tCode: 0x%X (should be 0xE)", tCode);
    ASFW_LOG_BUS_RESET_PACKET("  generation from packet: %u (arg: %u)", genFromPacket, newGeneration);

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

} // namespace ASFW::Async::Rx
