//
// StreamFormatParser.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Parser for IEC 61883-6 AM824 stream formats
// Extracts format details from AV/C command responses
//
// Reference: IEC 61883-6:2005 - Audio & Music Data Transmission Protocol
// Reference: TA Document 2001002 - AV/C Stream Format Information Specification
// Reference: FWA/src/FWA/PlugDetailParser.cpp:257-373
//

#pragma once

#include "StreamFormatTypes.hpp"
#include <cstdint>
#include <optional>
#include <vector>

namespace ASFW::Protocols::AVC::StreamFormats {

/// Parser for AV/C stream format responses
/// Handles various AM824 format encodings per IEC 61883-6
class StreamFormatParser {
public:
    //==========================================================================
    // Main Parsing Methods
    //==========================================================================

    /// Parse stream format from raw format block
    /// @param data Pointer to format block (starts at format_hierarchy byte)
    /// @param length Length of format block in bytes
    /// @return Parsed format or std::nullopt if invalid
    static std::optional<AudioStreamFormat> Parse(const uint8_t* data, size_t length);

    /// Parse compound AM824 format (subtype 0x40)
    /// Format: [90 40 rate sync channel_count [channel_formats...]]
    /// @param data Pointer to format block (starts at 0x90)
    /// @param length Length of format block
    /// @return Parsed format or std::nullopt if invalid
    static std::optional<AudioStreamFormat> ParseCompoundAM824(const uint8_t* data, size_t length);

    /// Parse simple AM824 6-byte format (subtype 0x00, 6 bytes)
    /// Format: [90 00 00 00 rate_nibble 00]
    /// @param data Pointer to format block (starts at 0x90)
    /// @param length Length of format block (should be 6)
    /// @return Parsed format or std::nullopt if invalid
    static std::optional<AudioStreamFormat> ParseSimpleAM824_6Byte(const uint8_t* data, size_t length);

    /// Parse simple AM824 3-byte format (subtype 0x00, 3 bytes)
    /// Format: [90 00 0F] (rate = don't care)
    /// @param data Pointer to format block (starts at 0x90)
    /// @param length Length of format block (should be 3)
    /// @return Parsed format or std::nullopt if invalid
    static std::optional<AudioStreamFormat> ParseSimpleAM824_3Byte(const uint8_t* data, size_t length);

    //==========================================================================
    // Field Extraction Helpers
    //==========================================================================

    /// Extract sample rate from rate byte
    /// @param rateByte IEC 61883-6 rate code (0x00-0x0F)
    /// @return Sample rate enum
    static SampleRate ExtractSampleRate(uint8_t rateByte);

    /// Extract sample rate from nibble (upper 4 bits)
    /// Used in some 6-byte simple formats
    /// @param byte Byte containing rate in upper nibble
    /// @return Sample rate enum
    static SampleRate ExtractSampleRateFromNibble(uint8_t byte);

    /// Extract synchronization mode from format bytes
    /// @param syncByte Byte containing sync flag (bit 2)
    /// @return Sync mode enum
    static SyncMode ExtractSyncMode(uint8_t syncByte);

    /// Parse channel format information from compound format
    /// @param data Pointer to format info fields (after byte 4)
    /// @param length Available data length
    /// @param numFields Number of format info fields (byte 4 value)
    /// @return Vector of channel format info
    static std::vector<ChannelFormatInfo> ParseChannelFormats(
        const uint8_t* data,
        size_t length,
        uint8_t numFields
    );

    //==========================================================================
    // Validation Helpers
    //==========================================================================

    /// Check if format hierarchy is AM824
    /// @param formatHierarchy First byte of format block
    /// @return true if 0x90 (AM824)
    static bool IsAM824(uint8_t formatHierarchy);

    /// Check if subtype indicates compound format
    /// @param subtype Second byte of format block
    /// @return true if 0x40 (compound)
    static bool IsCompound(uint8_t subtype);

    /// Check if subtype indicates simple format
    /// @param subtype Second byte of format block
    /// @return true if 0x00 (simple)
    static bool IsSimple(uint8_t subtype);

    /// Validate minimum format block size
    /// @param length Actual length
    /// @param minRequired Minimum required length
    /// @return true if valid
    static bool ValidateLength(size_t length, size_t minRequired);
};

} // namespace ASFW::Protocols::AVC::StreamFormats
