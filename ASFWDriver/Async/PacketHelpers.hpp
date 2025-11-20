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

    // Destination offset: bytes 8-13 (48-bit big-endian)
    return (static_cast<uint64_t>(header[8]) << 40) |
           (static_cast<uint64_t>(header[9]) << 32) |
           (static_cast<uint64_t>(header[10]) << 24) |
           (static_cast<uint64_t>(header[11]) << 16) |
           (static_cast<uint64_t>(header[12]) << 8) |
           (static_cast<uint64_t>(header[13]));
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
