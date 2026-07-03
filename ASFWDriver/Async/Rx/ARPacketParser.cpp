#include "ARPacketParser.hpp"
#include "../../Hardware/IEEE1394.hpp"
#include "../../Logging/LogConfig.hpp"
#include "../../Logging/Logging.hpp"

#ifdef ASFW_HOST_TEST
#include <libkern/OSByteOrder.h> // OSSwapLittleToHostInt32 for host tests
#else
#include <DriverKit/IOLib.h> // OSSwapLittleToHostInt32 for DriverKit
#endif

#include <cstring>

namespace ASFW::Async {

// Helper: Read little-endian uint32_t from AR DMA buffer
// OHCI AR DMA stores each quadlet in little-endian format in memory
static inline uint32_t le32_at(const uint8_t* p) {
    uint32_t v;
    __builtin_memcpy(&v, p, sizeof(v));
    return OSSwapLittleToHostInt32(v); // LE to host (no-op on arm64, documents intent)
}

std::optional<std::array<uint32_t, 2>>
ARPacketParser::ExtractPhyPacketQuadletsHostOrder(std::span<const uint8_t> header) {
    if (header.size() < 12) {
        return std::nullopt;
    }

    return std::array<uint32_t, 2>{
        le32_at(header.data() + 4),
        le32_at(header.data() + 8),
    };
}

std::expected<ARPacketParser::PacketInfo, ARPacketParser::ParseFailure>
ARPacketParser::ParseNext(std::span<const uint8_t> buffer, size_t offset) {
    const size_t bufferSize = buffer.size();

    if (buffer.empty() || offset + 8 > bufferSize) {
        return std::unexpected(ParseFailure::NeedMoreBytes);
    }

    const uint8_t* packetStart = buffer.data() + offset;

    // HEX DUMP: Complete AR packet as received (first 32 bytes or less)
    // V4/HEX: Only show raw dumps in debug mode or when explicitly enabled
    const size_t dumpSize = (bufferSize - offset > 32) ? 32 : (bufferSize - offset);
    ASFW_LOG_HEX(Async, "🔍 AR RX PACKET (offset=%zu size=%zu):", offset, dumpSize);
    for (size_t i = 0; i < dumpSize; i += 16) {
        const size_t chunkSize = (i + 16 <= dumpSize) ? 16 : (dumpSize - i);
        const uint8_t* bytes = packetStart + i;
        ASFW_LOG_HEX(Async,
                     "  [%02zu] %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  "
                     "%02X %02X %02X %02X",
                     i, chunkSize > 0 ? bytes[0] : 0, chunkSize > 1 ? bytes[1] : 0,
                     chunkSize > 2 ? bytes[2] : 0, chunkSize > 3 ? bytes[3] : 0,
                     chunkSize > 4 ? bytes[4] : 0, chunkSize > 5 ? bytes[5] : 0,
                     chunkSize > 6 ? bytes[6] : 0, chunkSize > 7 ? bytes[7] : 0,
                     chunkSize > 8 ? bytes[8] : 0, chunkSize > 9 ? bytes[9] : 0,
                     chunkSize > 10 ? bytes[10] : 0, chunkSize > 11 ? bytes[11] : 0,
                     chunkSize > 12 ? bytes[12] : 0, chunkSize > 13 ? bytes[13] : 0,
                     chunkSize > 14 ? bytes[14] : 0, chunkSize > 15 ? bytes[15] : 0);
    }

    // AR DMA stores each quadlet in little-endian format in memory
    // Read as LE quadlets to get IEEE 1394 packet format in host order
    const uint32_t q0 = le32_at(packetStart);
    const uint32_t q1 = (offset + 8 <= bufferSize) ? le32_at(packetStart + 4) : 0;

    // IEEE 1394 async packet format (quadlets in host order after LE load):
    // Q0: [destination_ID:16][tl:6][rt:2][tcode:4][priority/rcode:4]
    //     bits[31:16]        [15:10][9:8][7:4]   [3:0]
    // Per-byte view: header[0]=(tcode<<4)|pri, header[1] has tLabel bits[7:2]
    const uint8_t tCode = static_cast<uint8_t>((q0 >> 4) & 0xF);

    // V3: Show decoded values (more useful than raw hex for normal debugging)
    ASFW_LOG_V3(Async, "🔍 AR DECODED: q0=0x%08X q1=0x%08X tCode=0x%X", q0, q1, tCode);

    const size_t headerLength = GetHeaderLength(tCode);
    if (headerLength == 0) {
        ASFW_LOG_V0(Async, "❌ ARPacketParser::ParseNext: Unknown tCode=0x%X at offset %zu", tCode,
                    offset);
        return std::unexpected(ParseFailure::UnknownTCode);
    }

    // 1) Enough for header?
    if (offset + headerLength > bufferSize) {
        return std::unexpected(ParseFailure::NeedMoreBytes);
    }

    // 2) Find dataLength
    auto headerSpan = buffer.subspan(offset, headerLength);
    const size_t dataLength = GetDataLength(headerSpan, tCode);
    if (dataLength > kMaxAsyncPayloadBytes) {
        // A corrupt block header declaring a huge data_length must never be
        // treated as a stitchable fragment — it can never complete.
        // Cross-validated with Linux ohci.c handle_ar_packet:
        // payload_length > MAX_ASYNC_PAYLOAD → ar_context_abort("invalid packet length").
        ASFW_LOG_V0(
            Async,
            "❌ ARPacketParser::ParseNext: data_length %zu exceeds max %zu (tCode=0x%X offset=%zu)",
            dataLength, kMaxAsyncPayloadBytes, tCode, offset);
        return std::unexpected(ParseFailure::OversizedPayload);
    }
    const size_t quadletAlignedLen = ((headerLength + dataLength + 3) & ~size_t(3));

    // 3) Enough for header+data+trailer? The trailer is part of the DMA'd packet
    // unit, so its absence within the filled bytes means boundary fragment.
    if (offset + quadletAlignedLen + 4 > bufferSize) {
        return std::unexpected(ParseFailure::NeedMoreBytes);
    }

    // ---- Trailer (LE in memory)
    uint32_t trailer_le;
    __builtin_memcpy(&trailer_le, packetStart + quadletAlignedLen, 4);
    const uint32_t trailer = OSSwapLittleToHostInt32(trailer_le);
    const uint16_t xferStatus = static_cast<uint16_t>(trailer >> 16);
    const uint16_t timeStamp = static_cast<uint16_t>(trailer & 0xFFFF);

    // ---- rCode (only for response tCodes): extract from Q1 bits[15:12]
    // Per Linux packet-header-definitions.h: ASYNC_HEADER_Q1_RCODE_SHIFT = 12
    // IEEE 1394 format: Q1 = [source_ID:16][rcode:4][offset_high:12]
    uint8_t rCode = 0xFF; // 0xFF = "not present"
    if (tCode == kTCodeWriteResponse || tCode == kTCodeReadQuadletResponse ||
        tCode == kTCodeReadBlockResponse || tCode == kTCodeLockResponse) {
        rCode = static_cast<uint8_t>((q1 >> 12) & 0xF);
    }

    // Guard against garbage (all-zero header + zero trailer)
    if (q0 == 0 && q1 == 0 && xferStatus == 0 && timeStamp == 0) {
        return std::unexpected(ParseFailure::ZeroGarbage);
    }

    PacketInfo info{};
    info.packetStart = packetStart;
    info.headerLength = headerLength;
    info.dataLength = dataLength;
    info.totalLength = quadletAlignedLen + 4;
    info.tCode = tCode;
    info.rCode = rCode;
    info.xferStatus = xferStatus; // stored in 32-bit field; value range 0..0xFFFF
    info.timeStamp = timeStamp;   // stored in 32-bit field; value range 0..0xFFFF

    return info;
}

size_t ARPacketParser::GetHeaderLength(uint8_t tCode) {
    // Per Linux drivers/firewire/ohci.c:897-950 handle_ar_packet() switch statement
    // Header lengths match Linux fw_packet.header_length assignments
    // Cross-reference: OHCI §8.4 "Asynchronous Receive Data Formats"

    size_t length = 0;

    switch (tCode) {
    case kTCodeWriteQuadlet: // 0x0 TCODE_WRITE_QUADLET_REQUEST
        length = 16;         // 4 quadlets: header + data quadlet
        break;

    case kTCodeReadQuadletResponse: // 0x6 TCODE_READ_QUADLET_RESPONSE
        // IEEE 1394: Read Quadlet Response has 4 quadlets total (16 bytes)
        // Quadlet 0: destination/tLabel/tCode
        // Quadlet 1: source/rCode
        // Quadlet 2: reserved
        // Quadlet 3: DATA PAYLOAD (4 bytes) - embedded in header, not separate payload
        length = 16; // 4 quadlets including data
        break;

    case kTCodeReadBlock: // 0x5 TCODE_READ_BLOCK_REQUEST
        length = 16;      // 4 quadlets
        break;

    case kTCodeWriteBlock:        // 0x1 TCODE_WRITE_BLOCK_REQUEST
    case kTCodeReadBlockResponse: // 0x7 TCODE_READ_BLOCK_RESPONSE
    case kTCodeLockRequest:       // 0x9 TCODE_LOCK_REQUEST
    case kTCodeLockResponse:      // 0xB TCODE_LOCK_RESPONSE
        length = 16;              // 4 quadlets
        break;

    case kTCodeWriteResponse: // 0x2 TCODE_WRITE_RESPONSE
    case kTCodeReadQuadlet:   // 0x4 TCODE_READ_QUADLET_REQUEST
        length = 12;          // 3 quadlets (Linux: p.header_length = 12)
        break;

    case kTCodePhyPacket: // 0xE TCODE_LINK_INTERNAL/PHY
        // CRITICAL: Per Linux drivers/firewire/ohci.c handle_ar_packet
        // TCODE_LINK_INTERNAL (0xe): p.header_length = 12 (3 quadlets)
        // PHY packet structure per OHCI §8.4.2.3:
        //   Quadlet 0: tcode[31:28]=0xE, event[3:0]
        //   Quadlets 1-2: PHY payload; synthetic bus-reset markers reuse
        //   quadlet 1 for the selfIDGeneration[23:16] field.
        // Total: 12 bytes header + 4 bytes trailer = 16 bytes
        length = 12; // 3 quadlets (matches Linux!)
        break;

    default:
        // Any other tCode (incl. cycle start 0x8, iso 0xA — never delivered
        // to AR contexts with our LinkControl, same as Linux) is invalid
        // here; Linux ar_context_abort()s on it ("invalid tcode").
        ASFW_LOG_V0(Async, "❌ GetHeaderLength: Unknown tCode=0x%X", tCode);
        return 0; // Unknown tCode
    }

    ASFW_LOG_V3(Async, "GetHeaderLength(tCode=0x%X) → %zu bytes", tCode, length);
    return length;
}

size_t ARPacketParser::GetDataLength(std::span<const uint8_t> header, uint8_t tCode) {
    // Phase 2.2: header is now std::span for bounds-checked access
    // header points to wire-format packet header (big-endian bytes q0..q3)
    // Per Linux drivers/firewire/ohci.c:897-950 handle_ar_packet()
    // data_length extraction via async_header_get_data_length(p.header)
    // Source: Linux packet-header-definitions.h

    size_t dataLen = 0;

    switch (tCode) {
    case kTCodePhyPacket: // 0xE TCODE_LINK_INTERNAL
        // PHY packet structure per Linux & OHCI §8.4.2.3:
        // - Header: 12 bytes (3 quadlets) - all PHY data is part of header!
        // - Data: 0 bytes (no separate payload)
        // - Trailer: 4 bytes (xferStatus + timestamp)
        // TOTAL: 12 (hdr) + 0 (data) + 4 (trailer) = 16 bytes
        //
        // Linux: p.header_length=12, p.payload_length=0
        // All PHY-specific data is considered part of the header
        dataLen = 0; // No separate data payload!
        ASFW_LOG_V3(Async, "GetDataLength: PHY packet → 0 bytes data (all in 12-byte header)");
        break;

    case kTCodeWriteBlock:        // 0x1 TCODE_WRITE_BLOCK_REQUEST
    case kTCodeReadBlockResponse: // 0x7 TCODE_READ_BLOCK_RESPONSE
    case kTCodeLockRequest:       // 0x9 TCODE_LOCK_REQUEST
    case kTCodeLockResponse:      // 0xB TCODE_LOCK_RESPONSE
    {
        // Extract data_length from quadlet 3, bits[31:16]
        // Header quadlet 3 is at offset 12 (bytes 12-15)
        if (header.size() < 16) {
            ASFW_LOG_V0(Async,
                        "❌ GetDataLength: Header too small (%zu bytes) for block tCode=0x%X",
                        header.size(), tCode);
            return 0;
        }

        // Read q3 from AR DMA buffer (little-endian) and convert to host order
        const uint32_t q3 = le32_at(header.data() + 12);
        // Extract data_length from high 16 bits
        const uint16_t length = static_cast<uint16_t>((q3 >> 16) & 0xFFFF);
        dataLen = length;

        ASFW_LOG_V3(Async, "GetDataLength: Block tCode=0x%X q3=0x%08X (LE) → data_length=%u bytes",
                    tCode, q3, length);
        break;
    }

    case kTCodeReadQuadletResponse: // 0x6 TCODE_READ_QUADLET_RESPONSE
        // IEEE 1394: Data is embedded in header quadlet 3 (offset 12-15), not separate payload
        // Header length is 16 bytes, and q3 contains the 4-byte data value
        // Since data is part of header, dataLength is 0 (no separate payload follows)
        dataLen = 0;
        ASFW_LOG_V3(
            Async,
            "GetDataLength: tCode=0x6 (Read Quadlet Response) → 0 bytes (data in header q3)");
        break;

    case kTCodeWriteResponse: // 0x2 TCODE_WRITE_RESPONSE
        // No separate payload. (Write-compare is LOCK, not a write response.)
        dataLen = 0;
        ASFW_LOG_V3(Async, "GetDataLength: tCode=0x2 (Write Response) → 0 bytes");
        break;

    default:
        // No separate data (quadlet transactions, simple responses)
        // Per Linux: p.payload_length = 0 for these tCodes
        dataLen = 0;
        ASFW_LOG_V3(Async, "GetDataLength: tCode=0x%X → no payload (0 bytes)", tCode);
        break;
    }

    return dataLen;
}

} // namespace ASFW::Async
