//  AVCWireFormats.hpp
//  ASFWDriver
//
//  Wire formats for AV/C data serialization
//

#pragma once

#include <stdint.h>

namespace ASFW::UserClient::Wire {

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

/**
 * @brief Result of the signal-format probe (selector 64), polled via selector 39.
 *
 * The AV/C decode happens in the driver, not the app. DecodeSignalFormatProbe
 * validates that the reply echoed bytes 1-4 of the request, which is what stops
 * a reply to a different command being read as this one's answer — and the
 * request only exists driver-side. The app therefore receives a decoded verdict
 * rather than raw AV/C, and no AV/C knowledge is duplicated in Swift.
 *
 * `outcome` is SignalFormatProbeOutcome. `sampleRateHz` is meaningful only when
 * outcome == Decoded (0). `sfc` is retained even when reserved, for logging.
 * `fcpStatus` is the transport-level FCPStatus, so "the device never answered"
 * stays distinguishable from "the device refused".
 */
struct AVCSignalFormatProbeResultWire {
    uint8_t outcome;
    uint8_t sfc;
    uint8_t fcpStatus;
    uint8_t plugId;
    uint8_t isInputPlug;
    uint8_t _padding[3];
    uint32_t sampleRateHz;
} __attribute__((packed));

static_assert(sizeof(AVCSignalFormatProbeResultWire) == 12,
              "AVCSignalFormatProbeResultWire must be 12 bytes");

} // namespace ASFW::UserClient::Wire
