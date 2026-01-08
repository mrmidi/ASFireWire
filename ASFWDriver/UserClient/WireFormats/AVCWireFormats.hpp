//  AVCWireFormats.hpp
//  ASFWDriver
//
//  Wire formats for AV/C data serialization
//

#pragma once

#include <stdint.h>

namespace ASFW::UserClient::Wire {

/**
 * @brief Wire format for AV/C query response
 *
 * Structure:
 * - AVCQueryWire header
 * - For each unit:
 *   - AVCUnitWire (unit info)
 *   - Array of 'subunitCount' × AVCSubunitWire structures
 */
struct AVCQueryWire {
    uint32_t unitCount;       ///< Number of AV/C units
    uint32_t _padding;        ///< Padding for alignment
} __attribute__((packed));

static_assert(sizeof(AVCQueryWire) == 8, "AVCQueryWire must be 8 bytes");

/**
 * @brief Wire format for single AV/C unit
 *
 * Contains basic unit information: GUID, initialization status, subunit count
 */
struct AVCUnitWire {
    uint64_t guid;            ///< Unit GUID (from parent device)
    uint16_t nodeId;          ///< Current node ID
    uint8_t isInitialized;    ///< 1 if initialized, 0 otherwise
    uint8_t subunitCount;     ///< Number of discovered subunits
    uint32_t _padding;        ///< Padding for alignment
} __attribute__((packed));

static_assert(sizeof(AVCUnitWire) == 16, "AVCUnitWire must be 16 bytes");

/**
 * @brief Wire format for single AV/C subunit
 *
 * Contains subunit details: type, ID, plug counts
 */
struct AVCSubunitWire {
    uint8_t type;             ///< Subunit type (AVCSubunitType enum)
    uint8_t id;               ///< Subunit ID (0-7)
    uint8_t numDestPlugs;     ///< Destination (input) plugs
    uint8_t numSrcPlugs;      ///< Source (output) plugs
} __attribute__((packed));

static_assert(sizeof(AVCSubunitWire) == 4, "AVCSubunitWire must be 4 bytes");

/**
 * @brief Wire format for Music Subunit capabilities
 */
struct AVCMusicCapabilitiesWire {
    uint8_t hasAudioCapability;
    uint8_t hasMidiCapability;
    uint8_t hasSmpteCapability;
    uint8_t _reserved1;
    
    uint8_t audioInputPorts;
    uint8_t audioOutputPorts;
    uint8_t midiInputPorts;
    uint8_t midiOutputPorts;
    
    uint8_t smpteInputPorts;
    uint8_t smpteOutputPorts;
    uint8_t numSignalFormats;
    uint8_t numPlugs;
} __attribute__((packed));

static_assert(sizeof(AVCMusicCapabilitiesWire) == 12, "AVCMusicCapabilitiesWire must be 12 bytes");

static_assert(sizeof(AVCMusicCapabilitiesWire) == 12, "AVCMusicCapabilitiesWire must be 12 bytes");

struct AVCMusicSignalFormatWire {
    uint8_t format;
    uint8_t frequency;
    uint8_t isInput;
    uint8_t _padding;
} __attribute__((packed));

static_assert(sizeof(AVCMusicSignalFormatWire) == 4, "AVCMusicSignalFormatWire must be 4 bytes");

static_assert(sizeof(AVCMusicSignalFormatWire) == 4, "AVCMusicSignalFormatWire must be 4 bytes");

struct AVCMusicPlugNameWire {
    uint8_t plugID;
    uint8_t isInput;
    uint8_t nameLength;
    uint8_t _padding;
    char name[32]; // Fixed size for simplicity
} __attribute__((packed));

static_assert(sizeof(AVCMusicPlugNameWire) == 36, "AVCMusicPlugNameWire must be 36 bytes");

/**
 * @brief Wire format for Audio Subunit plug information
 */
struct AVCAudioPlugInfoWire {
    uint8_t plugNumber;
    uint8_t isInput;
    uint8_t formatType;      ///< 0x90 = AM824, etc.
    uint8_t formatSubtype;   ///< 0x00 = simple, 0x40 = compound
    uint8_t sampleRate;      ///< Sample rate code
    uint8_t numChannels;
    uint8_t _padding[2];
} __attribute__((packed));

static_assert(sizeof(AVCAudioPlugInfoWire) == 8, "AVCAudioPlugInfoWire must be 8 bytes");

/**
 * @brief Wire format for Audio Subunit capabilities
 */
struct AVCAudioCapabilitiesWire {
    uint8_t numInputPlugs;
    uint8_t numOutputPlugs;
    uint8_t _padding[2];
} __attribute__((packed));

static_assert(sizeof(AVCAudioCapabilitiesWire) == 4, "AVCAudioCapabilitiesWire must be 4 bytes");

} // namespace ASFW::UserClient::Wire

