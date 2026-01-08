//
// StreamFormatParser.cpp
// ASFWDriver - AV/C Protocol Layer
//
// Implementation of IEC 61883-6 AM824 stream format parser
//

#include "StreamFormatParser.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../../Logging/LogConfig.hpp"
#include <algorithm>



namespace ASFW::Protocols::AVC::StreamFormats {

//==============================================================================
// Main Parsing Method
//==============================================================================

std::optional<AudioStreamFormat> StreamFormatParser::Parse(
    const uint8_t* data,
    size_t length
) {
    if (!data || length < 2) {
        ASFW_LOG_ERROR(Discovery, "StreamFormatParser: Invalid input (data=%p, length=%zu)",
                      data, length);
        return std::nullopt;
    }

    uint8_t formatHierarchy = data[0];
    uint8_t subtype = data[1];

    // Check if AM824 format (0x90)
    if (!IsAM824(formatHierarchy)) {
        ASFW_LOG_WARNING(Discovery,
            "StreamFormatParser: Unsupported format hierarchy 0x%02x (expected AM824 0x90)",
            formatHierarchy);
        return std::nullopt;
    }

    // Dispatch based on subtype
    if (IsCompound(subtype)) {
        return ParseCompoundAM824(data, length);
    } else if (IsSimple(subtype)) {
        // Try 6-byte format first, fallback to 3-byte
        if (length >= 6) {
            return ParseSimpleAM824_6Byte(data, length);
        } else if (length >= 3) {
            return ParseSimpleAM824_3Byte(data, length);
        } else {
            ASFW_LOG_ERROR(Discovery,
                "StreamFormatParser: Simple AM824 too short (%zu bytes)", length);
            return std::nullopt;
        }
    } else {
        ASFW_LOG_WARNING(Discovery,
            "StreamFormatParser: Unsupported AM824 subtype 0x%02x", subtype);
        return std::nullopt;
    }
}

//==============================================================================
// Compound AM824 Parsing
//==============================================================================

std::optional<AudioStreamFormat> StreamFormatParser::ParseCompoundAM824(
    const uint8_t* data,
    size_t length
) {
    // Compound format structure (per IEC 61883-6 / TA 2001002):
    // [0] = 0x90 (format_hierarchy)
    // [1] = 0x40 (subtype - compound)
    // [2] = rate (sample rate code)
    // [3] = sync byte (bit 2 = sync flag)
    // [4] = number_of_format_information_fields (NOT total channels!)
    // [5...] = format info fields (2 bytes each: channel_count, format_code)

    if (!ValidateLength(length, 5)) {
        ASFW_LOG_ERROR(Discovery,
            "StreamFormatParser: Compound AM824 too short (%zu bytes, need >=5)", length);
        return std::nullopt;
    }

    AudioStreamFormat format;
    format.formatHierarchy = FormatHierarchy::kCompoundAM824;
    format.subtype = AM824Subtype::kCompound;
    format.sampleRate = ExtractSampleRate(data[2]);
    format.syncMode = ExtractSyncMode(data[3]);
    
    // FIX: Byte 4 is the number of format info fields, NOT total channels
    // Reference: Apple AVCVideoServices MusicSubunitController.cpp line 1294
    uint8_t numFormatFields = data[4];

    ASFW_LOG_V3(Discovery,
        "StreamFormatParser: Compound AM824 - rate=0x%02x (%u Hz), sync=%u, numFields=%u. Raw: %02x %02x %02x %02x %02x",
        data[2], format.GetSampleRateHz(),
        static_cast<uint8_t>(format.syncMode), numFormatFields,
        data[0], data[1], data[2], data[3], data[4]);

    // Parse channel formats if present
    if (length > 5 && numFormatFields > 0) {
        format.channelFormats = ParseChannelFormats(
            data + 5,
            length - 5,
            numFormatFields
        );
    }

    // FIX: Calculate total channels by summing up the parsed format fields
    format.totalChannels = 0;
    for (const auto& chFmt : format.channelFormats) {
        format.totalChannels += chFmt.channelCount;
    }

    // Store raw format block
    format.rawFormatBlock.assign(data, data + length);

    return format;
}

//==============================================================================
// Simple AM824 Parsing (6-byte)
//==============================================================================

std::optional<AudioStreamFormat> StreamFormatParser::ParseSimpleAM824_6Byte(
    const uint8_t* data,
    size_t length
) {
    // 6-byte simple format structure (device-specific quirks observed):
    // [0] = 0x90 (format_hierarchy)
    // [1] = 0x00 (subtype - simple)
    // [2] = may contain rate nibble (vendor quirk)
    // [3] = reserved
    // [4] = reserved (often 0x00/0x40 on Apogee)
    // [5] = rate byte (observed on Apogee)

    if (!ValidateLength(length, 6)) {
        ASFW_LOG_ERROR(Discovery,
            "StreamFormatParser: Simple 6-byte AM824 too short (%zu bytes, need 6)", length);
        return std::nullopt;
    }

    AudioStreamFormat format;
    format.formatHierarchy = FormatHierarchy::kAM824;
    format.subtype = AM824Subtype::kSimple;
    
    // Many OXFW/TA1394-style devices encode rate in the upper nibble of byte 2 (FDF rate control).
    SampleRate rate = SampleRate::kUnknown;

    const uint8_t fdfNibble = (data[2] >> 4) & 0x0F;
    const bool hasFdfNibble = fdfNibble != 0;
    if (hasFdfNibble) {
        rate = ExtractSampleRateFromNibble(data[2]);
    }

    // If nibble is absent/unknown/don't-care, try Music Subunit sample-rate codes in byte 5 (0x01=44.1, 0x02=48).
    if (rate == SampleRate::kUnknown || rate == SampleRate::kDontCare) {
        SampleRate musicRate = MusicSubunitCodeToSampleRate(data[5]);
        if (musicRate != SampleRate::kUnknown && musicRate != SampleRate::kDontCare) {
            rate = musicRate;
        }
    }

    // Final fallback (and override for legacy layouts): nibble in byte 4 if present.
    // Spec refinement: Only use byte 4 if we still haven't found a valid rate,
    // to avoid overriding valid rates with potential garbage data in this reserved field.
    if ((rate == SampleRate::kUnknown || rate == SampleRate::kDontCare) && (data[4] & 0xF0) != 0) {
        rate = ExtractSampleRateFromNibble(data[4]);
    }

    format.sampleRate = rate;

    format.syncMode = SyncMode::kNoSync; // Simple format doesn't specify sync
    format.totalChannels = 2; // Simple format typically stereo

    ASFW_LOG_V3(Discovery,
        "StreamFormatParser: Simple 6-byte AM824 - rateCode=0x%02x/0x%02x/0x%02x (%u Hz), channels=%u. Raw: %02x %02x %02x %02x %02x %02x",
        data[2], data[5], data[4], format.GetSampleRateHz(), format.totalChannels,
        data[0], data[1], data[2], data[3], data[4], data[5]);

    // Store raw format block
    format.rawFormatBlock.assign(data, data + length);

    return format;
}

//==============================================================================
// Simple AM824 Parsing (3-byte)
//==============================================================================

std::optional<AudioStreamFormat> StreamFormatParser::ParseSimpleAM824_3Byte(
    const uint8_t* data,
    size_t length
) {
    // 3-byte simple format structure:
    // [0] = 0x90 (format_hierarchy)
    // [1] = 0x00 (subtype - simple)
    // [2] = 0x0F (rate = don't care)

    if (!ValidateLength(length, 3)) {
        ASFW_LOG_ERROR(Discovery,
            "StreamFormatParser: Simple 3-byte AM824 too short (%zu bytes, need 3)", length);
        return std::nullopt;
    }

    AudioStreamFormat format;
    format.formatHierarchy = FormatHierarchy::kAM824;
    format.subtype = AM824Subtype::kSimple;
    format.sampleRate = SampleRate::kDontCare;  // Rate not specified
    format.syncMode = SyncMode::kNoSync;
    format.totalChannels = 2; // Simple format typically stereo

    ASFW_LOG_V3(Discovery,
        "StreamFormatParser: Simple 3-byte AM824 - rate=don't care, channels=%u",
        format.totalChannels);

    // Store raw format block
    format.rawFormatBlock.assign(data, data + length);

    return format;
}

//==============================================================================
// Field Extraction
//==============================================================================

SampleRate StreamFormatParser::ExtractSampleRate(uint8_t rateByte) {
    switch (rateByte) {
        case 0x00: return SampleRate::k22050Hz;
        case 0x01: return SampleRate::k24000Hz;
        case 0x02: return SampleRate::k32000Hz;
        case 0x03: return SampleRate::k44100Hz;
        case 0x04: return SampleRate::k48000Hz;
        case 0x05: return SampleRate::k96000Hz;
        case 0x06: return SampleRate::k176400Hz;
        case 0x07: return SampleRate::k192000Hz;
        case 0x0A: return SampleRate::k88200Hz;
        case 0x0F: return SampleRate::kDontCare;
        default:
            ASFW_LOG_WARNING(Discovery,
                "StreamFormatParser: Unknown sample rate code 0x%02x", rateByte);
            return SampleRate::kUnknown;
    }
}

SampleRate StreamFormatParser::ExtractSampleRateFromNibble(uint8_t byte) {
    // Extract upper 4 bits
    uint8_t nibble = (byte >> 4) & 0x0F;
    return ExtractSampleRate(nibble);
}

SyncMode StreamFormatParser::ExtractSyncMode(uint8_t syncByte) {
    // Bit 2 (0x04) indicates synchronization mode
    return (syncByte & 0x04) ? SyncMode::kSynchronized : SyncMode::kNoSync;
}

std::vector<ChannelFormatInfo> StreamFormatParser::ParseChannelFormats(
    const uint8_t* data,
    size_t length,
    uint8_t numFields
) {
    std::vector<ChannelFormatInfo> formats;

    // Each format info field is 2 bytes:
    // [0] = channel_count for this format code
    // [1] = format_code (IEC 61883-6 adaptation layer)
    // Reference: Apple AVCVideoServices MusicSubunitController.cpp lines 1380-1392

    size_t offset = 0;

    // FIX: Loop based on number of format fields, not accumulated channel count
    for (uint8_t i = 0; i < numFields; ++i) {
        // Ensure we have enough data left (2 bytes per field)
        if (offset + 2 > length) {
            ASFW_LOG_WARNING(Discovery,
                "StreamFormatParser: Truncated format list at field %u (offset %zu, length %zu)",
                i, offset, length);
            break;
        }

        ChannelFormatInfo info;
        info.channelCount = data[offset];
        info.formatCode = static_cast<StreamFormatCode>(data[offset + 1]);

        if (info.channelCount == 0) {
            ASFW_LOG_WARNING(Discovery,
                "StreamFormatParser: Invalid channel count 0 at field %u, offset %zu", i, offset);
            // Continue parsing remaining fields per Apple's behavior
        }

        formats.push_back(info);
        offset += 2;

        ASFW_LOG_V3(Discovery,
            "StreamFormatParser: Field %u - count=%u, code=0x%02x",
            i, info.channelCount, static_cast<uint8_t>(info.formatCode));
    }

    return formats;
}

//==============================================================================
// Validation Helpers
//==============================================================================

bool StreamFormatParser::IsAM824(uint8_t formatHierarchy) {
    // Standard AM824 format hierarchy (IEC 61883-6)
    // Note: Some legacy Oxford devices used 0x01, but we now reject these
    // to prevent parsing garbage data when the format offset is wrong.
    // If a specific device needs legacy support, it should be handled
    // with explicit device quirks, not by loosening validation.
    return formatHierarchy == 0x90;
}

bool StreamFormatParser::IsCompound(uint8_t subtype) {
    return subtype == 0x40;
}

bool StreamFormatParser::IsSimple(uint8_t subtype) {
    return subtype == 0x00 || subtype == 0x01 || subtype == 0x90;
}

bool StreamFormatParser::ValidateLength(size_t length, size_t minRequired) {
    if (length < minRequired) {
        ASFW_LOG_ERROR(Discovery,
            "StreamFormatParser: Invalid length %zu (need >=%zu)", length, minRequired);
        return false;
    }
    return true;
}

} // namespace ASFW::Protocols::AVC::StreamFormats
