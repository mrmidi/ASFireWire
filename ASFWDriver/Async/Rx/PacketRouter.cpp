#include "PacketRouter.hpp"

#include <cstring>

#include "ARPacketParser.hpp"
#include "../../Logging/Logging.hpp"
#include "../Tx/ResponseSender.hpp"

namespace ASFW::Async {

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

void PacketRouter::RoutePacket(ARContextType contextType, std::span<const uint8_t> packetData) {
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

        // Build zero-copy view over header and payload
        ARPacketView view;
        view.tCode = tCode;
        view.header = std::span<const uint8_t>(packetStart, headerLen);
        view.payload = std::span<const uint8_t>(packetStart + headerLen, dataLen);

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

        if (tCode < 16 && handlers[tCode]) {
            const ResponseCode rcode = handlers[tCode](view);

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

} // namespace ASFW::Async
