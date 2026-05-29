#include "PacketRouter.hpp"

#include <array>
#include <cstring>

#include "../../Common/FWCommon.hpp"
#include "ARPacketParser.hpp"
#include "../../Logging/Logging.hpp"
#include "../Tx/ResponseSender.hpp"
#include "../PacketHelpers.hpp"
#include "../../Debug/AsyncTraceCapture.hpp"
#include "../../Shared/ASFWDiagnosticsABI.h"
#include <DriverKit/IOLib.h>

namespace ASFW::Async {

namespace {

std::span<const uint8_t> CopyAlignedPayload(std::span<const uint8_t> source,
                                            std::array<uint8_t, ASFW::FW::MaxPayload::kS800>& scratch) {
    if (source.empty()) {
        return {};
    }

    if (source.size() > scratch.size()) {
        return {};
    }

    std::size_t index = 0;
    for (; index + sizeof(uint32_t) <= source.size(); index += sizeof(uint32_t)) {
        uint32_t quadlet = 0;
        __builtin_memcpy(&quadlet, source.data() + index, sizeof(uint32_t));
        __builtin_memcpy(scratch.data() + index, &quadlet, sizeof(uint32_t));
    }

    for (; index < source.size(); ++index) {
        scratch[index] = source[index];
    }

    return std::span<const uint8_t>(scratch.data(), source.size());
}

} // namespace

void PacketRouter::RegisterRequestHandler(uint8_t tCode, PacketHandler handler) {
    if (tCode >= 16) {
        ASFW_LOG(Async, "PacketRouter: invalid request tCode %u", tCode);
        return;
    }
    requestHandlers_[tCode] = std::move(handler);
}

void PacketRouter::RegisterResponseHandler(uint8_t tCode, PacketHandler handler) {
    if (tCode >= 16) {
        ASFW_LOG(Async, "PacketRouter: invalid response tCode %u", tCode);
        return;
    }
    responseHandlers_[tCode] = std::move(handler);
}

void PacketRouter::RoutePacket(ARContextType contextType, std::span<const uint8_t> packetData, uint32_t generation) {
    // Phase 2.2: Use std::span for type-safe buffer access
    if (packetData.empty()) {
        return;
    }

    // Select handler table based on context type
    const auto& handlers = (contextType == ARContextType::Request)
                               ? requestHandlers_
                               : responseHandlers_;

    const char* contextName = (contextType == ARContextType::Request)
                                  ? "Request"
                                  : "Response";

    // Parse packet stream - buffer may contain multiple packets
    // Per OHCI §8.4.2: AR buffers are packet streams, not single packets
    size_t offset = 0;
    const size_t bufferSize = packetData.size();

    while (offset < bufferSize) {
        auto packetInfoOpt = ARPacketParser::ParseNext(packetData, offset);
        if (!packetInfoOpt) {
            break;
        }

        const auto& packetInfo = *packetInfoOpt;

        const uint8_t* packetStart = packetInfo.packetStart;
        const size_t headerLen = packetInfo.headerLength;
        const size_t dataLen = packetInfo.dataLength;
        const uint8_t tCode = packetInfo.tCode;
        alignas(8) std::array<uint8_t, ASFW::FW::MaxPayload::kS800> payloadScratch{};

        // Build a dispatch view over the header and aligned payload bytes.
        ARPacketView view;
        view.tCode = tCode;
        view.header = std::span<const uint8_t>(packetStart, headerLen);
        if (dataLen > 0) {
            const auto payloadBytes = std::span<const uint8_t>(packetStart + headerLen, dataLen);
            view.payload = CopyAlignedPayload(payloadBytes, payloadScratch);
            if (view.payload.empty()) {
                ASFW_LOG(Async,
                         "PacketRouter: payload %zu exceeds aligned scratch buffer for tCode=0x%x",
                         dataLen,
                         tCode);
                offset += packetInfo.totalLength;
                continue;
            }
        } else {
            view.payload = {};
        }

        // Trailer fields – use low 16 bits for xferStatus/timeStamp
        view.xferStatus = static_cast<uint16_t>(packetInfo.xferStatus & 0xFFFF);
        view.timeStamp  = static_cast<uint16_t>(packetInfo.timeStamp  & 0xFFFF);

        if (headerLen >= 6) {
            // We can safely use the header span we already built
            view.destID   = ExtractDestID(view.header);
            view.sourceID = ExtractSourceID(view.header);
            view.tLabel   = ExtractTLabel(view.header);
        } else {
            // PHY or malformed packet – leave IDs/label at 0
            view.destID   = 0;
            view.sourceID = 0;
            view.tLabel   = 0;
        }

        // Capture incoming transaction
        CaptureIncomingEvent(contextType, view, generation);

        if (tCode < 16 && handlers[tCode]) {
            const ResponseCode rcode = handlers[tCode](view, generation);

            if (contextType == ARContextType::Request &&
                responseSender_ &&
                rcode != ResponseCode::NoResponse) {
                responseSender_->SendWriteResponse(view, rcode);
            }
        } else {
            ASFW_LOG(Async, "PacketRouter: unhandled AR %{public}s packet tCode=0x%x",
                     contextName, tCode);
        }

        offset += packetInfo.totalLength;
    }
}

void PacketRouter::ClearAllHandlers() {
    for (auto& handler : requestHandlers_) {
        handler = nullptr;
    }
    for (auto& handler : responseHandlers_) {
        handler = nullptr;
    }
}

uint8_t PacketRouter::ExtractTCode(std::span<const uint8_t> header) noexcept {
    // Phase 2.2: Bounds-checked access via std::span
    if (header.size() < 1) return 0;
    // AR DMA writes quadlets in little-endian host byte order.
    // Wire byte3 (tCode | pri) appears at header[0]. Extract high nibble.
    return static_cast<uint8_t>((header[0] >> 4) & 0x0F);
}

uint16_t PacketRouter::ExtractSourceID(std::span<const uint8_t> header) noexcept {
    // Phase 2.2: Bounds-checked access via std::span
    if (header.size() < 8) return 0;
    // OHCI AR DMA stores quadlets in little-endian format in memory
    // IEEE 1394 Q1 (at memory offset 4-7): [srcID:16][rCode:4][offset_high:12]
    // After LE load, srcID is at bytes [6-7] (big-endian within the quadlet)
    // For Q1 = FF FF C2 FF in memory:
    //   Wire Q1 was: FF C2 FF FF (big-endian)
    //   srcID on wire: FF C2
    //   In memory bytes [6-7]: C2 FF (reversed by LE load)
    // So we need: (header[7] << 8) | header[6] to get 0xFFC2
    return static_cast<uint16_t>((header[7] << 8) | header[6]);
}

uint16_t PacketRouter::ExtractDestID(std::span<const uint8_t> header) noexcept {
    // Phase 2.2: Bounds-checked access via std::span
    if (header.size() < 4) return 0;
    // OHCI AR DMA stores quadlets in little-endian format in memory
    // IEEE 1394 Q0 (at memory offset 0-3): [destID:16][tLabel:6][rt:2][tCode:4][pri:4]
    // After LE load, destID is at bytes [2-3] (big-endian within the quadlet)
    // For Q0 = 10 7D C0 FF in memory:
    //   Wire Q0 was: FF C0 7D 10 (big-endian)
    //   destID on wire: FF C0
    //   In memory bytes [2-3]: C0 FF (reversed by LE load)
    // So we need: (header[3] << 8) | header[2] to get 0xFFC0
    return static_cast<uint16_t>((header[3] << 8) | header[2]);
}

uint8_t PacketRouter::ExtractTLabel(std::span<const uint8_t> header) noexcept {
    // Phase 2.2: Bounds-checked access via std::span
    if (header.size() < 2) return 0;
    // OHCI AR DMA stores quadlets in little-endian format in memory
    // IEEE 1394 Q0: [destID:16][tLabel:6][rt:2][tCode:4][pri:4]
    // After LE load, tLabel is at header[1] bits[7:2]
    return static_cast<uint8_t>((header[1] >> 2) & 0x3F);
}

void PacketRouter::CaptureIncomingEvent(ARContextType contextType, const ARPacketView& view, uint32_t generation) noexcept {
    if (traceCapture_) {
        ASFWDiagAsyncEvent event{};
        
        static mach_timebase_info_data_t timebase{};
        if (timebase.denom == 0) {
            mach_timebase_info(&timebase);
        }
        event.timestampNs = (mach_absolute_time() * timebase.numer) / timebase.denom;
        event.generation = generation;
        event.direction = 0; // 0 for RX (incoming)
        event.context = (contextType == ARContextType::Request) ? 0 : 1;
        event.tLabel = view.tLabel;
        event.tCode = view.tCode;
        event.sourceId = view.sourceID;
        event.destinationId = view.destID;
        
        // Extract address from header if header is long enough
        if (view.header.size() >= 12) {
            event.address = ExtractDestOffset(view.header);
        }
        
        // Extract quadletData if it's a quadlet read/write request/response
        if (view.tCode == 0x0 || view.tCode == 0x6) {
            if (view.payload.size() >= 4) {
                std::memcpy(&event.quadletData, view.payload.data(), 4);
                event.quadletData = OSSwapBigToHostInt32(event.quadletData);
            } else if (view.header.size() >= 16) {
                std::memcpy(&event.quadletData, view.header.data() + 12, 4);
                event.quadletData = OSSwapLittleToHostInt32(event.quadletData);
            }
        }
        
        event.payloadBytes = static_cast<uint32_t>(view.payload.size());
        event.ackCode = view.xferStatus & 0x1F; 
        
        if (view.header.size() >= 8) {
            event.rCode = (view.header[5] >> 4) & 0x0F;
        }
        
        event.speed = 0; // S100
        
        traceCapture_->CaptureEvent(event);
    }

    if (contextType == ARContextType::Request && csrStats_) {
        const uint64_t destOffset = ExtractDestOffset(view.header);
        const uint8_t tCode = view.tCode;
        
        if (destOffset >= 0xFFFFF0000000ULL && destOffset <= 0xFFFFF000FFFFULL) {
            bool handled = false;
            
            if (destOffset >= 0xFFFFF0000400ULL && destOffset <= 0xFFFFF00007FFULL) {
                if (tCode == 0x4 || tCode == 0x5) {
                    csrStats_->inboundConfigROMReads++;
                    handled = true;
                }
            } else if (destOffset == 0xFFFFF0000000ULL) {
                // +0x000 is STATE_CLEAR (IEEE 1212 / Apple IOFireWireFamilyCommon.h:544)
                if (tCode == 0x0 || tCode == 0x1) {
                    csrStats_->inboundStateClearWrites++;
                    handled = true;
                }
            } else if (destOffset == 0xFFFFF0000004ULL) {
                // +0x004 is STATE_SET
                if (tCode == 0x0 || tCode == 0x1) {
                    csrStats_->inboundStateSetWrites++;
                    handled = true;
                }
            } else if (destOffset == 0xFFFFF000021CULL) {
                if (tCode == 0x4) {
                    csrStats_->inboundBusManagerIdReads++;
                    handled = true;
                } else if (tCode == 0x9) {
                    csrStats_->inboundBusManagerIdLocks++;
                    handled = true;
                }
            } else if (destOffset == 0xFFFFF0000220ULL) {
                if (tCode == 0x4) {
                    csrStats_->inboundBandwidthReads++;
                    handled = true;
                } else if (tCode == 0x9) {
                    csrStats_->inboundBandwidthLocks++;
                    handled = true;
                }
            } else if (destOffset == 0xFFFFF0000224ULL || destOffset == 0xFFFFF0000228ULL) {
                if (tCode == 0x4) {
                    csrStats_->inboundChannelReads++;
                    handled = true;
                } else if (tCode == 0x9) {
                    csrStats_->inboundChannelLocks++;
                    handled = true;
                }
            } else if (destOffset == 0xFFFFF0000234ULL) {
                // BROADCAST_CHANNEL is at +0x234 (Apple IOFireWireFamilyCommon.h:586)
                if (tCode == 0x4) {
                    csrStats_->inboundBroadcastChannelReads++;
                    handled = true;
                } else if (tCode == 0x0 || tCode == 0x1) {
                    csrStats_->inboundBroadcastChannelWrites++;
                    handled = true;
                }
            } else if (destOffset >= 0xFFFFF0001000ULL && destOffset <= 0xFFFFF00013FFULL) {
                if (tCode == 0x4 || tCode == 0x5) {
                    csrStats_->inboundTopologyMapReads++;
                    handled = true;
                }
            } else if (destOffset >= 0xFFFFF0002000ULL && destOffset <= 0xFFFFF00027FFULL) {
                if (tCode == 0x4 || tCode == 0x5) {
                    csrStats_->inboundSpeedMapReads++;
                    handled = true;
                }
            } else if (destOffset == 0xFFFFF0000B00ULL || destOffset == 0xFFFFF0000D00ULL) {
                // FCP_COMMAND (+0xB00) / FCP_RESPONSE (+0xD00) are AV/C transaction space,
                // not CSR registers (Apple IOFireWireFamilyCommon.h:593-594). They live in the
                // initial register window but must not be counted as "unsupported CSR".
                handled = true;
            }

            if (!handled) {
                csrStats_->unsupportedCSRRequests++;
            }
        }
    }
}

} // namespace ASFW::Async
