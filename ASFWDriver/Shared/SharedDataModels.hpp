//
//  SharedDataModels.hpp
//  ASFWDriver
//
//  Shared Data Models between DriverKit and Swift
//  These structures must be byte-aligned and padded manually to ensure
//  compatibility between ARM64 (Driver) and the User Client.
//

#pragma once

#include <stdint.h>

namespace ASFW {
namespace Shared {

// -----------------------------------------------------------------------------
// AV/C Unit Information
// -----------------------------------------------------------------------------

struct AVCSubunitInfoWire {
    uint8_t  type;
    uint8_t  subunitID;
    uint8_t  numSrcPlugs;
    uint8_t  numDestPlugs;
} __attribute__((packed));

struct AVCUnitInfoWire {
    uint64_t guid;
    uint16_t nodeID;
    uint16_t vendorID;
    uint16_t modelID;
    uint8_t  subunitCount;
    uint8_t  isoInputPlugs;
    uint8_t  isoOutputPlugs;
    uint8_t  extInputPlugs;
    uint8_t  extOutputPlugs;
    uint8_t  _reserved;    // Padding to 20 bytes
    // Followed by variable length AVCSubunitInfoWire array
    // AVCSubunitInfoWire subunits[0];
} __attribute__((packed));

// -----------------------------------------------------------------------------
// Music Subunit Capabilities
// -----------------------------------------------------------------------------

/// Individual channel detail within a signal block
/// Contains Music Plug ID and channel name from MusicPlugInfo
struct ChannelDetailWire {
    uint16_t musicPlugID;      ///< Music Plug ID from ClusterInfo signal
    uint8_t  position;         ///< Position within cluster (channel index)
    uint8_t  nameLength;       ///< Length of name string
    char     name[32];         ///< Channel name (e.g. "Analog Out 1")
} __attribute__((packed));

static_assert(sizeof(ChannelDetailWire) == 36, "ChannelDetailWire must be 36 bytes");

/// Signal block with nested channel details
/// Represents one encoding group (e.g. "2ch MBLA")
struct SignalBlockWire {
    uint8_t formatCode;        ///< 0x06=MBLA, 0x00=IEC60958, 0x40=SyncStream, etc.
    uint8_t channelCount;      ///< Total channels in this block
    uint8_t numChannelDetails; ///< Count of ChannelDetailWire entries that follow
    uint8_t _padding;          ///< Alignment
    // Followed by ChannelDetailWire[numChannelDetails]
} __attribute__((packed));

static_assert(sizeof(SignalBlockWire) == 4, "SignalBlockWire must be 4 bytes");

/// Supported stream format entry (from 0xBF STREAM FORMAT queries)
struct SupportedFormatWire {
    uint8_t sampleRateCode;    ///< SampleRate enum: 0=32k, 1=44.1k, 2=48k, 3=88.2k, 4=96k, 5=176.4k, 6=192k, 0xFF=don't care
    uint8_t formatCode;        ///< StreamFormatCode: 0x06=MBLA, 0x40=SyncStream, etc.
    uint8_t channelCount;      ///< Total channels in this format
    uint8_t _padding;          ///< Alignment
} __attribute__((packed));

static_assert(sizeof(SupportedFormatWire) == 4, "SupportedFormatWire must be 4 bytes");

/// Plug information with nested signal blocks and supported formats
struct PlugInfoWire {
    uint8_t plugID;
    uint8_t isInput;           ///< 1 = input (destination), 0 = output (source)
    uint8_t type;              ///< MusicPlugType: Audio=0x00, MIDI=0x01, Sync=0x80
    uint8_t numSignalBlocks;   ///< Count of SignalBlockWire entries that follow
    uint8_t nameLength;        ///< Length of plug name
    char    name[32];          ///< Plug name (e.g. "Analog Out")
    uint8_t numSupportedFormats; ///< Count of SupportedFormatWire entries (max 32)
    uint8_t _padding[2];       ///< Alignment padding (total: 40 bytes)
    // Followed by:
    //   SignalBlockWire[numSignalBlocks]
    //     Each SignalBlockWire followed by ChannelDetailWire[numChannelDetails]
    //   SupportedFormatWire[numSupportedFormats]
} __attribute__((packed));

static_assert(sizeof(PlugInfoWire) == 40, "PlugInfoWire must be 40 bytes");

/// Music Subunit capabilities header
struct AVCMusicCapabilitiesWire {
    // Capability Flags (1 byte)
    uint8_t hasAudio : 1;
    uint8_t hasMIDI  : 1;
    uint8_t hasSMPTE : 1;
    uint8_t _reservedFlags : 5;
    
    // Global Rates (from first valid plug)
    uint8_t currentRate;          ///< Current sample rate code (0x03=44.1k, 0x04=48k)
    uint32_t supportedRatesMask;  ///< Bitmask: Bit 3=44.1k, 4=48k, 5=96k, 0xA=88.2k
    uint8_t _padding[2];          ///< Alignment to 8 bytes

    // Port Counts (from descriptor)
    uint8_t audioInputPorts;
    uint8_t audioOutputPorts;
    uint8_t midiInputPorts;
    uint8_t midiOutputPorts;
    uint8_t smpteInputPorts;
    uint8_t smpteOutputPorts;
    
    // Structure counts
    uint8_t numPlugs;             ///< Count of PlugInfoWire entries
    uint8_t _reserved;            ///< Reserved (was numChannels, now nested)
    uint8_t _padding2[2];         ///< Alignment padding
    
    // Variable length data follows:
    //   PlugInfoWire[numPlugs]
    //     Each PlugInfoWire followed by SignalBlockWire[numSignalBlocks]
    //       Each SignalBlockWire followed by ChannelDetailWire[numChannelDetails]
} __attribute__((packed));

} // namespace Shared

// -----------------------------------------------------------------------------
// Metrics Snapshot (for UserClient export to Swift GUI)
// -----------------------------------------------------------------------------
namespace Metrics {

/// Isoch Receive metrics snapshot for GUI display
/// Wire format - must match Swift exactly
struct IsochRxSnapshot {
    // Counters
    uint64_t totalPackets;
    uint64_t dataPackets;     // 80-byte with samples
    uint64_t emptyPackets;    // 16-byte empty
    uint64_t drops;           // DBC discontinuities
    uint64_t errors;          // CIP parse errors
    
    // Latency histogram [<100µs, 100-500µs, 500-1000µs, >1000µs]
    uint64_t latencyHist[4];
    
    // Last poll cycle
    uint32_t lastPollLatencyUs;
    uint32_t lastPollPackets;
    
    // CIP header snapshot
    uint8_t cipSID;
    uint8_t cipDBS;
    uint8_t cipFDF;
    uint8_t _pad1;
    uint16_t cipSYT;
    uint8_t cipDBC;
    uint8_t _pad2;
} __attribute__((packed));

static_assert(sizeof(IsochRxSnapshot) == 88, "IsochRxSnapshot must be 88 bytes");

} // namespace Metrics
} // namespace ASFW

