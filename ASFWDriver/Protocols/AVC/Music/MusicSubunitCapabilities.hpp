//
// MusicSubunitCapabilities.hpp
// ASFWDriver - AV/C Protocol Layer
//
// Capabilities for Music Subunit (Audio/MIDI/SMPTE)
// Ported from FWA, with Bug Fixes from Phase 2
//

#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <string>
#include "../AVCDefs.hpp"

namespace ASFW::Protocols::AVC::Music {

/// Audio Sample Format
struct AudioSampleFormat {
    uint8_t raw[3]; // 3 bytes from AM824 or similar
    // TODO: Add parsing helpers if needed
};

/// Music Subunit Capabilities
/// Reference: TA Document 2001007 - Music Subunit Specification
struct MusicSubunitCapabilities {
    // Version
    uint8_t musicSubunitVersion{0};

    // Basic Capability Flags
    bool hasGeneralCapability{false};
    bool hasAudioCapability{false};
    bool hasMidiCapability{false};
    bool hasSmpteTimeCodeCapability{false};
    bool hasSampleCountCapability{false};
    bool hasAudioSyncCapability{false};

    // General Capabilities
    std::optional<uint8_t> transmitCapabilityFlags;
    std::optional<uint8_t> receiveCapabilityFlags;
    
    // Bug Fix #1: latencyCapability must be uint32_t (4 bytes per spec)
    // Reference: TA 2001007, Section 5.2.1, Table 5.5
    std::optional<uint32_t> latencyCapability;

    // Audio Capabilities
    // Bug Fix #2: Channel counts must be uint16_t (2 bytes per spec)
    // Reference: TA 2001007, Section 5.2.2, Table 5.7
    std::optional<uint16_t> maxAudioInputChannels;
    std::optional<uint16_t> maxAudioOutputChannels;
    std::optional<std::vector<AudioSampleFormat>> availableAudioFormats;

    // MIDI Capabilities
    // Bug Fix #3: MIDI port counts must be uint16_t (2 bytes per spec)
    // Reference: TA 2001007, Section 5.2.3, Table 5.9
    std::optional<uint16_t> maxMidiInputPorts;
    std::optional<uint16_t> maxMidiOutputPorts;
    std::optional<uint8_t> midiVersionMajor;
    std::optional<uint8_t> midiVersionMinor;
    std::optional<uint8_t> midiAdaptationLayerVersion;

    // SMPTE Capabilities
    std::optional<uint8_t> smpteTimeCodeCapabilityFlags;

    // Sample Count Capabilities
    std::optional<uint8_t> sampleCountCapabilityFlags;

    // Audio SYNC Capabilities
    std::optional<uint8_t> audioSyncCapabilityFlags;

    //==========================================================================
    // Device Identity (populated from parent FWDevice/Config ROM)
    //==========================================================================
    
    std::string vendorName;                      // From FWDevice::GetVendorName()
    std::string modelName;                       // From FWDevice::GetModelName()
    uint64_t guid{0};                            // From FWDevice::GetGUID()

    //==========================================================================
    // Audio Configuration (derived from MusicSubunit discovery)
    //==========================================================================
    
    /// Supported sample rates in Hz (extracted from supportedFormats)
    std::vector<double> supportedSampleRates;
    
    /// Current sample rate in Hz (from a decoded active format). Zero means
    /// unknown; generic discovery must not invent a rate.
    double currentSampleRate{0.0};
    
    /// Plug names (first input/output plug names for stream labeling)
    /// Defaults to "Input"/"Output" if no name available from device
    std::string inputPlugName = "Input";
    std::string outputPlugName = "Output";

    //==========================================================================
    // Capability Flag Helpers
    //==========================================================================
    
    bool HasGeneralCapability() const { return hasGeneralCapability; }
    bool HasAudioCapability() const { return hasAudioCapability; }
    bool HasMidiCapability() const { return hasMidiCapability; }
    bool HasSmpteTimeCodeCapability() const { return hasSmpteTimeCodeCapability; }
    bool HasSampleCountCapability() const { return hasSampleCountCapability; }
    bool HasAudioSyncCapability() const { return hasAudioSyncCapability; }

    //==========================================================================
    // General Capabilities Helpers
    // Bug Fix #3: Corrected bit positions - bit 1 for blocking, bit 0 for non-blocking
    // Reference: TA 2001007, Section 5.2.1, Table 5.5
    //==========================================================================
    
    bool SupportsBlockingTransmit() const {
        return transmitCapabilityFlags && (*transmitCapabilityFlags & 0x02); // Bit 1
    }
    
    bool SupportsNonBlockingTransmit() const {
        return transmitCapabilityFlags && (*transmitCapabilityFlags & 0x01); // Bit 0
    }
    
    bool SupportsBlockingReceive() const {
        return receiveCapabilityFlags && (*receiveCapabilityFlags & 0x02); // Bit 1
    }
    
    bool SupportsNonBlockingReceive() const {
        return receiveCapabilityFlags && (*receiveCapabilityFlags & 0x01); // Bit 0
    }

    //==========================================================================
    // SMPTE Capabilities Helpers
    //==========================================================================
    
    bool SupportsSmpteTransmit() const {
        return smpteTimeCodeCapabilityFlags && (*smpteTimeCodeCapabilityFlags & 0x02);
    }
    
    bool SupportsSmpteReceive() const {
        return smpteTimeCodeCapabilityFlags && (*smpteTimeCodeCapabilityFlags & 0x01);
    }

    //==========================================================================
    // Sample Count Capabilities Helpers
    //==========================================================================
    
    bool SupportsSampleCountTransmit() const {
        return sampleCountCapabilityFlags && (*sampleCountCapabilityFlags & 0x02);
    }
    
    bool SupportsSampleCountReceive() const {
        return sampleCountCapabilityFlags && (*sampleCountCapabilityFlags & 0x01);
    }

    //==========================================================================
    // Audio SYNC Capabilities Helpers
    //==========================================================================
    
    bool SupportsAudioSyncBus() const {
        return audioSyncCapabilityFlags && (*audioSyncCapabilityFlags & 0x01);
    }
    
    bool SupportsAudioSyncExternal() const {
        return audioSyncCapabilityFlags && (*audioSyncCapabilityFlags & 0x02);
    }
};

} // namespace ASFW::Protocols::AVC::Music
