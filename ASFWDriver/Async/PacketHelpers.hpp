//
// PacketHelpers.hpp
// ASFWDriver - Async Packet Utilities
//
// Helper functions for extracting fields from IEEE 1394 packet headers
//

#pragma once

#include <cstdint>
#include <span>

namespace ASFW::Async {

//==============================================================================
// Packet Header Field Extraction
//==============================================================================

/// Extract destination offset from async packet header
///
/// Per IEEE 1394-1995 §6.2.1, destination_offset is at bytes 8-13 (48-bit).
///
/// @param header Packet header bytes (big-endian, minimum 16 bytes)
/// @return Destination offset (48-bit address), or 0 if header too short
inline uint64_t ExtractDestOffset(std::span<const uint8_t> header) {
    if (header.size() < 16) {
        return 0;
    }

    // IEEE 1394 Block Write packet format (wire, big-endian):
    //   Q0: [destination_ID:16][tLabel:6][rt:2][tCode:4][pri:4]
    //   Q1: [source_ID:16][rCode:4][destination_offset_high:12]
    //   Q2: [destination_offset_low:32]
    //   Q3: [data_length:16][extended_tcode:16]
    //
    // OHCI AR DMA stores each quadlet in little-endian format in memory.
    // So for Q1 wire value [srcID:16][rCode:4][offset_high:12]:
    //   Wire: [srcID_high:8][srcID_low:8][rCode:4|offset_high[11:8]:4][offset_high[7:0]:8]
    //   Memory bytes[4-7]: [offset_high[7:0]][rCode|offset_high[11:8]][srcID_low][srcID_high]
    //
    // For Q2 wire value [offset_low:32]:
    //   Wire: [offset_low[31:24]][offset_low[23:16]][offset_low[15:8]][offset_low[7:0]]
    //   Memory bytes[8-11]: [offset_low[7:0]][offset_low[15:8]][offset_low[23:16]][offset_low[31:24]]
    
    // Extract 12-bit offset_high from Q1 bytes [4-5]
    // header[4] = offset_high[7:0]
    // header[5] = (rCode << 4) | offset_high[11:8]
    uint64_t offset_high_low = header[4];  // bits [7:0]
    uint64_t offset_high_high = header[5] & 0x0F;  // bits [11:8]
    uint64_t offset_high_12bit = (offset_high_high << 8) | offset_high_low;
    
    // Sign-extend 12-bit to 16-bit (if bit 11 is set, extend with 1s)
    uint64_t offset_high = offset_high_12bit;
    if (offset_high_12bit & 0x800) {
        offset_high |= 0xF000;  // Sign extend
    }
    
    // Extract 32-bit offset_low from Q2 bytes [8-11] (reverse byte order for LE)
    uint64_t offset_low = (static_cast<uint64_t>(header[11]) << 24) |
                          (static_cast<uint64_t>(header[10]) << 16) |
                          (static_cast<uint64_t>(header[9]) << 8) |
                          static_cast<uint64_t>(header[8]);
    
    // Combine into 48-bit address
    return (offset_high << 32) | offset_low;
}

/// Extract data length from block write/read packet header
///
/// Per IEEE 1394-1995 §6.2.4, data_length is at bytes 14-15 (16-bit).
///
/// @param header Packet header bytes (big-endian, minimum 16 bytes)
/// @return Data length in bytes, or 0 if header too short
inline uint16_t ExtractDataLength(std::span<const uint8_t> header) {
    if (header.size() < 16) {
        return 0;
    }

    // Data length: bytes 14-15 (16-bit big-endian)
    return (static_cast<uint16_t>(header[14]) << 8) |
           static_cast<uint16_t>(header[15]);
}

/// Extract extended transaction code from packet header
///
/// For block write/read packets, extended_tcode is at byte 7.
///
/// @param header Packet header bytes (big-endian, minimum 16 bytes)
/// @return Extended tcode, or 0 if header too short
inline uint8_t ExtractExtendedTCode(std::span<const uint8_t> header) {
    if (header.size() < 16) {
        return 0;
    }

    // Extended tcode: byte 7
    return header[7];
}

} // namespace ASFW::Async
