#include "PacketRouter.hpp"

#include <cstring>

#include "ARPacketParser.hpp"
#include "../../Logging/Logging.hpp"

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
    const size_t packetSize = packetData.size();  // Cache for loop
    while (offset < packetSize) {
        // Extract next packet from buffer (Phase 2.2: pass span)
        auto packetInfo = ARPacketParser::ParseNext(packetData, offset);
        if (!packetInfo) {
            // No more valid packets in buffer
            break;
        }

        const uint8_t tCode = packetInfo->tCode;

        // Build zero-copy packet view
        ARPacketView view;
        view.tCode = tCode;
        view.header = std::span<const uint8_t>(
            packetInfo->packetStart,
            packetInfo->headerLength);
        view.payload = std::span<const uint8_t>(
            packetInfo->packetStart + packetInfo->headerLength,
            packetInfo->dataLength);

        // Extract additional fields from header (Phase 2.2: pass subspan)
        if (packetInfo->headerLength >= 6) {
            // Create subspan for header extraction (bounds-checked)
            auto headerSpan = packetData.subspan(offset, std::min(packetInfo->headerLength, packetSize - offset));
            view.destID = ExtractDestID(headerSpan);
            view.sourceID = ExtractSourceID(headerSpan);
            view.tLabel = ExtractTLabel(headerSpan);
        } else {
            // Short header (PHY packet or malformed)
            view.destID = 0;
            view.sourceID = 0;
            view.tLabel = 0;
        }

        // Lookup and invoke handler
        if (tCode < 16 && handlers[tCode]) {
            // Dispatch to registered handler
            handlers[tCode](view);
        } else {
            // No handler registered - log warning
            ASFW_LOG(Async, "PacketRouter: unhandled AR %{public}s packet tCode=0x%x",
                     contextName, tCode);
        }

        // Advance to next packet
        // Per OHCI §8.4.2: packet length = header + data + 4-byte trailer
        offset += packetInfo->totalLength;
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
    return static_cast<uint16_t>((header[6] << 8) | header[7]);
}

uint16_t PacketRouter::ExtractDestID(std::span<const uint8_t> header) noexcept {
    // Phase 2.2: Bounds-checked access via std::span
    if (header.size() < 4) return 0;
    // OHCI AR DMA stores quadlets in little-endian format in memory
    // IEEE 1394 Q0 (at memory offset 0-3): [destID:16][tLabel:6][rt:2][tCode:4][pri:4]
    // After LE load, destID is at bytes [2-3] (big-endian within the quadlet)
    return static_cast<uint16_t>((header[2] << 8) | header[3]);
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
