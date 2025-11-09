#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::Async {

// Per OHCI §8.4.2: AR buffers contain stream of packets, each with:
//   - Packet header (variable length based on tCode)
//   - Packet data (optional, based on tCode and data_length field)
//   - Packet trailer (4 bytes: xferStatus[31:16] | timeStamp[15:0])
//
// CRITICAL: Hardware may write MULTIPLE packets into single buffer
// Software must parse buffer as stream, not treat entire buffer as one packet
//
// Per Linux drivers/firewire/ohci.c:1656-1720 handle_ar_packet():
//   - Walk buffer offset-by-offset
//   - Extract tCode from first byte (wire format)
//   - Compute header_length from tCode (IEEE 1394 §6.2)
//   - Read data_length from header fields (if applicable)
//   - Packet length = header_length + data_length + 4 (trailer)
//   - Advance cursor and repeat until buffer exhausted

class ARPacketParser {
public:
    struct PacketInfo {
        const uint8_t* packetStart;  // Points to first byte of packet header
        size_t headerLength;         // Header size in bytes
        size_t dataLength;           // Payload size in bytes (0 for no-data packets)
        size_t totalLength;          // header + data + 4 (trailer)
        uint8_t tCode;               // Transaction code from wire byte 0 [7:4]
        uint8_t rCode;                // Response code from q1 [31:28] (0xFF = not present, for response tCodes 0x2, 0x6, 0x7, 0xB)
        uint32_t xferStatus;         // From trailer quadlet
        uint32_t timeStamp;          // From trailer quadlet
    };

    // Parse next packet from buffer at given offset (Phase 2.2: std::span)
    // Returns packet info if valid packet found, nullopt if buffer exhausted/malformed
    static std::optional<PacketInfo> ParseNext(std::span<const uint8_t> buffer,
                                               size_t offset);

    // Get IEEE 1394 async header length from tCode
    // Per IEEE 1394-1995 Table 6-2 and OHCI §8.4.2
    static size_t GetHeaderLength(uint8_t tCode);

    // Extract data_length field from packet header (Phase 2.2: std::span)
    // Per IEEE 1394-1995 §6.2: data_length in quadlet 3, bits[31:16]
    static size_t GetDataLength(std::span<const uint8_t> header, uint8_t tCode);

private:
    // tCode values per IEEE 1394-1995 Table 6-1
    static constexpr uint8_t kTCodeWriteQuadlet = 0x0;
    static constexpr uint8_t kTCodeWriteBlock = 0x1;
    static constexpr uint8_t kTCodeWriteResponse = 0x2;
    static constexpr uint8_t kTCodeReadQuadlet = 0x4;
    static constexpr uint8_t kTCodeReadBlock = 0x5;
    static constexpr uint8_t kTCodeReadQuadletResponse = 0x6;
    static constexpr uint8_t kTCodeReadBlockResponse = 0x7;
    static constexpr uint8_t kTCodeCycleStart = 0x8;
    static constexpr uint8_t kTCodeLockRequest = 0x9;
    static constexpr uint8_t kTCodeIsochronousBlock = 0xA;
    static constexpr uint8_t kTCodeLockResponse = 0xB;
    static constexpr uint8_t kTCodePhyPacket = 0xE;
};

} // namespace ASFW::Async
