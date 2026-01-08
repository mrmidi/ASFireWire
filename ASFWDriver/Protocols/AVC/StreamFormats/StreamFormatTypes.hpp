//
// StreamFormatTypes.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Stream format types, enums, and structures for IEC 61883-6 AM824 formats
// Reference: TA Document 2001002 - AV/C Stream Format Information Specification
// Reference: IEC 61883-6 - Audio & Music Data Transmission Protocol
//

#pragma once

#include <cstdint>
#include <vector>
#include <optional>

namespace ASFW::Protocols::AVC::StreamFormats {

//==============================================================================
// Format Type Enums (IEC 61883-6)
//==============================================================================

/// Top-level format hierarchy codes
enum class FormatHierarchy : uint8_t {
    kAM824 = 0x90,              ///< IEC 61883-6 AM824 (most common for audio)
    kCompoundAM824 = 0x90,      ///< Same as AM824 but with compound structure
    kLegacyGeneric = 0x01,      ///< Legacy "Generic" format (Oxford chipsets)
    kLegacySimple = 0x00,       ///< Legacy "Simple" format (Oxford chipsets)
    kAudioPack = 0x20,          ///< Audio Pack format (rare)
    kFloatingPoint = 0x21,      ///< 32-bit floating point (rare)
    kUnknown = 0xFF
};

/// AM824 format subtypes
enum class AM824Subtype : uint8_t {
    kSimple = 0x00,             ///< Simple format (3 or 6 bytes)
    kCompound = 0x40,           ///< Compound format with channel details
    kUnknown = 0xFF
};

/// Stream format codes (IEC 61883-6 adaptation layers)
/// These identify the audio encoding within AM824 streams
enum class StreamFormatCode : uint8_t {
    // IEC 61883-6:2005 Standard Codes
    kIEC60958_3 = 0x00,         ///< Consumer Audio (S/PDIF, AES/EBU)
    kMBLA = 0x06,               ///< Multi-bit Linear Audio (24-bit PCM)
    kHighPrecisionMBLA = 0x07,  ///< High Precision MBLA (>24-bit, up to 192-bit)
    kOneBitAudio = 0x08,        ///< DSD (Direct Stream Digital for SACD)
    kEncodedAudio = 0x09,       ///< Encoded audio (e.g. DST for SACD)
    kMIDI = 0x0D,               ///< MIDI Conformant Data
    kSMPTE = 0x0E,              ///< SMPTE Time Code
    kSampleCount = 0x0F,        ///< Sample Count
    kFloatingPoint32 = 0x10,    ///< 32-bit IEEE 754 floating point
    kDVDAudio = 0x11,           ///< DVD-Audio specific formats
    kBluRayAudio = 0x12,        ///< Blu-ray Disc audio formats (up to 7.1)
    kUnknown = 0xFF
};

/// Sample rates (IEC 61883-6 frequency codes)
enum class SampleRate : uint8_t {
    k22050Hz = 0x00,
    k24000Hz = 0x01,
    k32000Hz = 0x02,
    k44100Hz = 0x03,
    k48000Hz = 0x04,
    k96000Hz = 0x05,
    k176400Hz = 0x06,
    k192000Hz = 0x07,
    k88200Hz = 0x0A,
    kDontCare = 0x0F,           ///< Rate not specified / don't care
    kUnknown = 0xFF
};

/// Convert sample rate enum to Hz
inline uint32_t SampleRateToHz(SampleRate rate) {
    switch (rate) {
        case SampleRate::k22050Hz:  return 22050;
        case SampleRate::k24000Hz:  return 24000;
        case SampleRate::k32000Hz:  return 32000;
        case SampleRate::k44100Hz:  return 44100;
        case SampleRate::k48000Hz:  return 48000;
        case SampleRate::k88200Hz:  return 88200;
        case SampleRate::k96000Hz:  return 96000;
        case SampleRate::k176400Hz: return 176400;
        case SampleRate::k192000Hz: return 192000;
        default: return 0;
    }
}

/// Synchronization mode
enum class SyncMode : uint8_t {
    kNoSync = 0,                ///< Not synchronized
    kSynchronized = 1,          ///< Synchronized to external clock
    kUnknown = 0xFF
};

/// Convert Music Subunit specific frequency code (0xA0/0xA1 command) to SampleRate
// Note: These differ from the IEC 61883-6 codes used in AM824 stream formats.
inline SampleRate MusicSubunitCodeToSampleRate(uint8_t freq) {
    switch (freq) {
        case 0x00: return SampleRate::k32000Hz;
        case 0x01: return SampleRate::k44100Hz;
        case 0x02: return SampleRate::k48000Hz;
        case 0x03: return SampleRate::k88200Hz;
        case 0x04: return SampleRate::k96000Hz;
        case 0x05: return SampleRate::k176400Hz;
        case 0x06: return SampleRate::k192000Hz;
        default: return SampleRate::kUnknown;
    }
}

//==============================================================================
// Stream Format Structures
//==============================================================================

/// Channel format information (for compound AM824)
struct ChannelFormatInfo {
    uint8_t channelCount{0};                    ///< Number of channels
    StreamFormatCode formatCode{StreamFormatCode::kUnknown}; ///< Encoding type

    /// Per-channel details (from ClusterInfo signals + MusicPlugInfo names)
    struct ChannelDetail {
        uint16_t musicPlugID{0xFFFF};   ///< Music Plug ID from ClusterInfo signal
        uint8_t position{0};            ///< Position within cluster (channel index)
        std::string name;               ///< Channel name from MusicPlugInfo ("Analog Out 1")
        
        bool HasName() const { return !name.empty(); }
    };
    std::vector<ChannelDetail> channels;  ///< Individual channel details

    bool IsValid() const {
        return channelCount > 0 && formatCode != StreamFormatCode::kUnknown;
    }
};

/// Complete audio stream format information
struct AudioStreamFormat {
    // Format hierarchy
    FormatHierarchy formatHierarchy{FormatHierarchy::kUnknown};
    AM824Subtype subtype{AM824Subtype::kUnknown};

    // Audio parameters
    SampleRate sampleRate{SampleRate::kUnknown};
    SyncMode syncMode{SyncMode::kUnknown};
    uint8_t totalChannels{0};

    // Channel details (for compound format)
    std::vector<ChannelFormatInfo> channelFormats;

    // Raw format block (for future parsing or debugging)
    std::vector<uint8_t> rawFormatBlock;

    bool IsValid() const {
        return formatHierarchy != FormatHierarchy::kUnknown &&
               subtype != AM824Subtype::kUnknown;
    }

    bool IsCompound() const {
        return subtype == AM824Subtype::kCompound;
    }

    bool IsSimple() const {
        return subtype == AM824Subtype::kSimple;
    }

    uint32_t GetSampleRateHz() const {
        return SampleRateToHz(sampleRate);
    }
};

//==============================================================================
// Connection Information
//==============================================================================

/// Plug direction
enum class PlugDirection : uint8_t {
    kInput = 0x00,              ///< Destination plug (input)
    kOutput = 0x01,             ///< Source plug (output)
};

/// Source subunit type for connections
enum class SourceSubunitType : uint8_t {
    kAudio = 0x01,
    kMusic = 0x0C,
    kUnit = 0xFF,               ///< Unit-level connection
    kNotConnected = 0xFE,       ///< Not connected (special value)
    kUnknown = 0xFF
};

/// Connection information (SIGNAL SOURCE response)
/// Describes which source plug feeds a destination plug
struct ConnectionInfo {
    SourceSubunitType sourceSubunitType{SourceSubunitType::kUnknown};
    uint8_t sourceSubunitID{0xFF};
    uint8_t sourcePlugNumber{0xFF};

    bool IsConnected() const {
        return sourceSubunitType != SourceSubunitType::kNotConnected &&
               sourceSubunitType != SourceSubunitType::kUnknown;
    }

    bool IsUnitConnection() const {
        return sourceSubunitType == SourceSubunitType::kUnit;
    }
};

/// Destination plug connection info (Music Subunit specific)
/// From DESTINATION PLUG CONFIGURE command
struct DestPlugConnectionInfo {
    uint8_t sourcePlugNumber{0xFF};
    uint8_t destinationPlugNumber{0xFF};
    bool isConnected{false};

    bool IsValid() const {
        return sourcePlugNumber != 0xFF && destinationPlugNumber != 0xFF;
    }
};

//==============================================================================
// Plug Information Structure
//==============================================================================

//==============================================================================
// Music Subunit Specific Enums (Legacy / Spec 2001007)
//==============================================================================

/// Music Subunit Plug Usages (Descriptor field)
enum class MusicSubunitPlugUsage : uint8_t {
    kIsochStream    = 0x00,
    kAsynchStream   = 0x01,
    kMIDI           = 0x02,
    kSync           = 0x03,
    kAnalog         = 0x04,
    kDigital        = 0x05,
    kUnknown        = 0xFF
};

/// Music Port Types (e.g. for MusicPlugInfo blocks)
enum class MusicPortType : uint8_t {
    kSpeaker        = 0x00,
    kHeadPhone      = 0x01,
    kMicrophone     = 0x02,
    kLine           = 0x03,
    kSPDIF          = 0x04,
    kADAT           = 0x05,
    kTDIF           = 0x06,
    kMADI           = 0x07,
    kAnalog         = 0x08,
    kDigital        = 0x09,
    kMIDI           = 0x0A,
    kAES_EBU        = 0x0B,
    kNoType         = 0xFF
};

/// Music Plug Locations (Spatial)
enum class MusicPlugLocation : uint8_t {
    kLeftFront          = 0x01,
    kRightFront         = 0x02,
    kCenterFront        = 0x03,
    kLowFreqEnhance     = 0x04,
    kLeftSurround       = 0x05,
    kRightSurround      = 0x06,
    kLeftOfCenter       = 0x07,
    kRightOfCenter      = 0x08,
    kSurround           = 0x09,
    kSideLeft           = 0x0A,
    kSideRight          = 0x0B,
    kTop                = 0x0C,
    kBottom             = 0x0D,
    kLeftFrontEffect    = 0x0E,
    kRightFrontEffect   = 0x0F,
    kUnknown            = 0xFF
};

/// Music Subunit Plug Types
enum class MusicPlugType : uint8_t {
    kAudio          = 0x00,
    kMIDI           = 0x01,
    kSMPTE          = 0x02,
    kSampleCount    = 0x03,
    kSync           = 0x80,
    kUnknown        = 0xFF
};

/// Complete plug information combining format, connection, and metadata
struct PlugInfo {
    // Basic identification
    uint8_t plugID{0xFF};
    PlugDirection direction{PlugDirection::kInput};
    MusicPlugType type{MusicPlugType::kUnknown};
    std::string name;

    // Current format
    std::optional<AudioStreamFormat> currentFormat;

    // Supported formats (queried via STREAM FORMAT SUPPORT)
    std::vector<AudioStreamFormat> supportedFormats;

    // Connection topology
    std::optional<ConnectionInfo> connectionInfo;
    std::optional<DestPlugConnectionInfo> destPlugConnectionInfo;

    bool IsValid() const { return plugID != 0xFF; }
    bool IsInput() const { return direction == PlugDirection::kInput; }
    bool IsOutput() const { return direction == PlugDirection::kOutput; }
};

} // namespace ASFW::Protocols::AVC::StreamFormats
