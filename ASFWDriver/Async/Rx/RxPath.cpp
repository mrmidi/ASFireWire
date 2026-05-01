#include "RxPath.hpp"
#include "ARPacketParser.hpp"
#include "../../Hardware/OHCIEventCodes.hpp"
#include "../../Hardware/OHCIDescriptors.hpp"
#include "../../Hardware/IEEE1394.hpp"
#include "../../Debug/BusResetPacketCapture.hpp"
#include "../../Phy/PhyPackets.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Common/DMASafeCopy.hpp"
#include "../../Common/FWCommon.hpp"
#include "../ResponseCode.hpp"
#include "../../Common/FWCommon.hpp"

#include <DriverKit/IOLib.h>
#include <array>
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
    if (!isRunning) {
        return;
    }

    currentBusResetCapture_ = busResetCapture;
    ProcessRequestInterrupts();

    if (is_bus_reset_in_progress.load(std::memory_order_acquire)) {
        ASFW_LOG(Async, "RxPath: Skipping AR Response during bus reset");
        currentBusResetCapture_ = nullptr;
        return;
    }

    ProcessResponseInterrupts(busResetCapture);
    currentBusResetCapture_ = nullptr;
}

void RxPath::ProcessRequestInterrupts() {
    auto* ctx = &arRequestContext_;
    constexpr const char* ctxLabel = "AR Request";

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
    auto commitConsumed = [&](size_t descriptorIndex, size_t consumedBytes) {
        const kern_return_t kr = ctx->CommitConsumed(descriptorIndex, consumedBytes);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Async,
                     "RxPath: Failed to commit %zu bytes for descriptor %zu in %{public}s (kr=0x%08x)",
                     consumedBytes,
                     descriptorIndex,
                     ctxLabel,
                     kr);
        }
    };

    uint32_t buffersProcessed = 0;
    while (auto bufferInfo = ctx->Dequeue()) {
        const auto& info = *bufferInfo;
        ++buffersProcessed;

        ASFW_LOG_HEX(Async,
                     "RxPath AR Request Buffer #%u: vaddr=%p startOffset=%zu size=%zu index=%zu",
                     buffersProcessed,
                     info.virtualAddress,
                     info.startOffset,
                     info.bytesFilled,
                     info.descriptorIndex);

        if (!info.virtualAddress) {
            ASFW_LOG_HEX(Async,
                         "RxPath AR Request Buffer #%u: NULL virtual address, recycling",
                         buffersProcessed);
            recycle(info.descriptorIndex);
            continue;
        }

        const uint8_t* bufferStart = info.virtualAddress;
        const std::size_t bufferSize = info.bytesFilled;
        if (bufferSize == 0 || bufferSize <= info.startOffset) {
            recycle(info.descriptorIndex);
            continue;
        }

        DumpRequestBuffer(buffersProcessed, bufferStart, bufferSize);

        const uint8_t* newDataStart = bufferStart + info.startOffset;
        const std::size_t newDataSize = bufferSize - info.startOffset;
        ASFW_LOG_HEX(Async,
                     "RxPath AR Request Buffer #%u: routing %zu NEW bytes from offset %zu",
                     buffersProcessed,
                     newDataSize,
                     info.startOffset);
        packetRouter_.RoutePacket(
            ARContextType::Request,
            std::span<const uint8_t>(newDataStart, newDataSize));
        commitConsumed(info.descriptorIndex, bufferSize);
    }

    ASFW_LOG_V2(Async, "RxPath: Processed %u buffers from %{public}s",
                buffersProcessed, ctxLabel);
}

void RxPath::ProcessResponseInterrupts(Debug::BusResetPacketCapture* busResetCapture) {
    auto* ctx = &arResponseContext_;
    constexpr const char* ctxLabel = "AR Response";
    constexpr ARContextType ctxType = ARContextType::Response;
    alignas(4) std::array<uint8_t, ARPacketParser::kMaxPacketBytes> stitchedPacket{};

    DumpResponseInterruptState(*ctx);

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
    auto commitConsumed = [&](size_t descriptorIndex, size_t consumedBytes) {
        const kern_return_t kr = ctx->CommitConsumed(descriptorIndex, consumedBytes);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(Async,
                     "RxPath: Failed to commit %zu bytes for descriptor %zu in %{public}s (kr=0x%08x)",
                     consumedBytes,
                     descriptorIndex,
                     ctxLabel,
                     kr);
        }
    };

    uint32_t buffersProcessed = 0;
    uint32_t packetsFound = 0;
    while (auto bufferInfo = ctx->Dequeue()) {
        const auto& info = *bufferInfo;
        ++buffersProcessed;

        if (!info.virtualAddress) {
            recycle(info.descriptorIndex);
            continue;
        }

        const uint8_t* bufferStart = info.virtualAddress;
        const std::size_t bufferSize = info.bytesFilled;
        const std::size_t startOffset = info.startOffset;
        if (bufferSize == 0 || bufferSize <= startOffset) {
            recycle(info.descriptorIndex);
            continue;
        }

        const uint8_t* newDataStart = bufferStart + startOffset;
        const std::size_t newDataSize = bufferSize - startOffset;
        LogResponseNewData(newDataStart, startOffset, bufferSize);

        std::size_t offset = startOffset;
        bool consumedStitchedPacket = false;
        bool committedBeforeBoundaryRetry = false;
        while (offset < bufferSize) {
            auto packetInfo =
                ARPacketParser::ParseNext(std::span<const uint8_t>(bufferStart, bufferSize), offset);
            if (!packetInfo.has_value()) {
                if (offset > startOffset) {
                    commitConsumed(info.descriptorIndex, offset);
                    committedBeforeBoundaryRetry = true;
                }

                const size_t stitchedBytes = ctx->CopyReadableBytes(stitchedPacket);
                if (stitchedBytes > 0) {
                    auto stitchedInfo = ARPacketParser::ParseNext(
                        std::span<const uint8_t>(stitchedPacket.data(), stitchedBytes), 0);
                    if (stitchedInfo.has_value() &&
                        stitchedInfo->totalLength <= stitchedBytes &&
                        stitchedInfo->totalLength > 0) {
                        ++packetsFound;
                        ProcessReceivedPacket(ctxType, *stitchedInfo, busResetCapture);
                        const kern_return_t consumeKr =
                            ctx->ConsumeReadableBytes(stitchedInfo->totalLength);
                        if (consumeKr != kIOReturnSuccess) {
                            ASFW_LOG(Async,
                                     "RxPath: Failed to consume stitched %{public}s packet (%zu bytes, kr=0x%08x)",
                                     ctxLabel,
                                     stitchedInfo->totalLength,
                                     consumeKr);
                        } else {
                            ASFW_LOG_V2(Async,
                                        "AR Response: stitched packet across buffer boundary (%zu bytes)",
                                        stitchedInfo->totalLength);
                            consumedStitchedPacket = true;
                        }
                    }
                }
                break;
            }

            ++packetsFound;
            ProcessReceivedPacket(ctxType, *packetInfo, busResetCapture);
            offset += packetInfo->totalLength;
        }

        if (consumedStitchedPacket) {
            continue;
        }

        ASFW_LOG_V2(Async,
                    "✅ RxPath AR/RSP: Processed %zu NEW bytes from buffer[%zu] "
                    "(offset %zu→%zu, total=%zu) - leaving descriptor owned by hardware",
                    newDataSize,
                    info.descriptorIndex,
                    startOffset,
                    offset,
                    bufferSize);
        if (!committedBeforeBoundaryRetry) {
            commitConsumed(info.descriptorIndex, offset);
        }
        if (offset == startOffset && startOffset < bufferSize) {
            ASFW_LOG_V1(Async,
                        "AR Response: parser made no progress in buffer[%zu] at offset %zu/%zu; "
                        "preserving tail until hardware appends more bytes",
                        info.descriptorIndex,
                        startOffset,
                        bufferSize);
        }
        // AR response traffic has proven to be stable when we keep the descriptor live
        // and rely on BufferRing::Dequeue() to auto-advance once the next buffer gains
        // data. Recycling here immediately after each response packet caused `no9` to
        // regress into global Config ROM read timeouts even though QRresp packets were
        // still present on the wire.
    }

    ASFW_LOG_V2(Async, "RxPath: Processed %u packets in %u buffers from %{public}s",
                packetsFound, buffersProcessed, ctxLabel);

    if (buffersProcessed == 0 && packetsFound == 0) {
        ASFW_LOG_V3(Async, "AR Response: No packets read for this interrupt");
        DumpEmptyResponseBuffer(*ctx);
    }
}

void RxPath::DumpRequestBuffer(uint32_t buffersProcessed,
                               const uint8_t* bufferStart,
                               size_t bufferSize) const {
    if (bufferSize < 32) {
        return;
    }

    ASFW_LOG_HEX(Async,
                 "RxPath AR Request Buffer #%u first 128 bytes (showing 32-byte chunks):",
                 buffersProcessed);
    const size_t dumpSize = (bufferSize < 128) ? bufferSize : 128;
    for (size_t i = 0; i < dumpSize; i += 32) {
        const size_t chunkSize = ((i + 32) <= dumpSize) ? 32 : (dumpSize - i);
        if (chunkSize < 16) {
            continue;
        }

        const uint8_t* chunk = bufferStart + i;
        ASFW_LOG_HEX(Async,
                     "  [%04zx] %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                     i,
                     chunk[0], chunk[1], chunk[2], chunk[3],
                     chunk[4], chunk[5], chunk[6], chunk[7],
                     chunk[8], chunk[9], chunk[10], chunk[11],
                     chunk[12], chunk[13], chunk[14], chunk[15]);
        if (chunkSize == 32) {
            ASFW_LOG_HEX(Async,
                         "  [%04zx] %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                         i + 16,
                         chunk[16], chunk[17], chunk[18], chunk[19],
                         chunk[20], chunk[21], chunk[22], chunk[23],
                         chunk[24], chunk[25], chunk[26], chunk[27],
                         chunk[28], chunk[29], chunk[30], chunk[31]);
        }
    }
}

void RxPath::DumpResponseInterruptState(ARResponseContext& ctx) const {
    auto& bufferRing = ctx.GetBufferRing();
    auto* descBase = bufferRing.DescriptorBaseVA();
    if (descBase) {
        const auto& desc = descBase[0];
        const uint16_t resCount = HW::AR_resCount(desc);
        const uint16_t reqCount = static_cast<uint16_t>(desc.control & 0xFFFF);
        const uint16_t xferStatus = HW::AR_xferStatus(desc);
        ASFW_LOG_HEX(Async, "🔍 AR/RSP interrupt: Descriptor[0] BEFORE cache invalidation:");
        ASFW_LOG_HEX(Async, "    statusWord=0x%08X control=0x%08X",
                     desc.statusWord, desc.control);
        ASFW_LOG_HEX(Async, "    resCount=%u reqCount=%u xferStatus=0x%04X %{public}s",
                     resCount,
                     reqCount,
                     xferStatus,
                     (resCount == reqCount) ? "(EMPTY)" : "(FILLED)");
    }

    uint8_t* firstBuffer = bufferRing.GetBufferAddress(0);
    if (!firstBuffer) {
        return;
    }

    const uint8_t* bytes = firstBuffer;
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

void RxPath::LogResponseNewData(const uint8_t* newDataStart,
                                size_t startOffset,
                                size_t bufferSize) const {
    const std::size_t newDataSize = bufferSize - startOffset;
    if (newDataSize < 16) {
        return;
    }

    uint32_t q0 = 0;
    uint32_t q1 = 0;
    __builtin_memcpy(&q0, newDataStart, 4);
    __builtin_memcpy(&q1, newDataStart + 4, 4);
    q0 = OSSwapLittleToHostInt32(q0);
    q1 = OSSwapLittleToHostInt32(q1);

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
    ASFW_LOG_HEX(Async,
                 "AR/RSP NEW q0=0x%08X q1=0x%08X  → tCode=0x%X, tLabel=%u, rCode=0x%X",
                 q0, q1, tCode_dbg, tLabel_dbg, rCode_dbg);
}

void RxPath::DumpEmptyResponseBuffer(ARResponseContext& ctx) const {
    auto& bufferRing = ctx.GetBufferRing();
    uint8_t* firstBuffer = bufferRing.GetBufferAddress(0);
    if (!firstBuffer) {
        ASFW_LOG_HEX(Async, "⚠️  AR Response: Cannot get buffer address for dump");
        return;
    }

    const uint8_t* bytes = firstBuffer;
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
    Common::CopyFromQuadletAlignedDeviceMemory(
        std::span<uint8_t>(payloadCopy, payloadLen),
        payloadPtr);

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
    std::array<uint32_t, 2> rawQuadlets{};
    std::memcpy(rawQuadlets.data(), quadlets, sizeof(uint32_t) * rawQuadlets.size());
    const uint32_t q0 =
        OSSwapLittleToHostInt32(rawQuadlets[0]);  // LE bytes → BE wire format
    const uint32_t q1 =
        OSSwapLittleToHostInt32(rawQuadlets[1]);  // LE bytes → BE wire format

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
