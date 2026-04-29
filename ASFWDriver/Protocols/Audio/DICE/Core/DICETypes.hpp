// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// DICETypes.hpp - Core DICE protocol types
// Reference: TCAT DICE protocol, snd-firewire-ctl-services/protocols/dice/src/tcat.rs

#pragma once

#include "../../../../Async/AsyncTypes.hpp"

#include <cstddef>
#include <cstdint>

namespace ASFW::Audio::DICE {

// ============================================================================
// DICE Address Space
// ============================================================================

/// Base address for DICE CSR space (IEEE 1394 private space)
constexpr uint64_t kDICEBaseAddress = 0xFFFFE0000000ULL;

/// Base offset for the TCAT extension space relative to DICE CSR space.
constexpr uint32_t kDICEExtensionOffset = 0x00200000U;

[[nodiscard]] constexpr uint64_t DICEAbsoluteAddress(uint32_t offset) noexcept {
    return kDICEBaseAddress + offset;
}

[[nodiscard]] inline ::ASFW::Async::FWAddress MakeDICEAddress(uint32_t offset) noexcept {
    const uint64_t address = DICEAbsoluteAddress(offset);
    return ::ASFW::Async::FWAddress{::ASFW::Async::FWAddress::AddressParts{
        .addressHi = static_cast<uint16_t>((address >> 32U) & 0xFFFFU),
        .addressLo = static_cast<uint32_t>(address & 0xFFFFFFFFU),
    }};
}

// ============================================================================
// Section Definition
// ============================================================================

/// A section in DICE control/status register space
/// Each section has an offset and size (both in bytes, converted from quadlets)
struct Section {
    uint32_t offset{0};  ///< Offset from base address (bytes)
    uint32_t size{0};    ///< Size of section (bytes)
    
    /// Size of section descriptor in wire format (2 quadlets)
    static constexpr size_t kWireSize = 8;
    
    /// Parse section from big-endian wire format
    static Section Deserialize(const uint8_t* data) {
        Section s;
        // Offset and size are stored as quadlet counts, multiply by 4
        s.offset = ((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]) * 4;
        s.size   = ((data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7]) * 4;
        return s;
    }

    static Section FromWire(const uint8_t* data) {
        return Deserialize(data);
    }
};

// ============================================================================
// General Sections (standard DICE layout)
// ============================================================================

/// Standard DICE sections present in all DICE devices
struct GeneralSections {
    Section global;           ///< Global settings (clock, sample rate, nickname)
    Section txStreamFormat;   ///< TX stream format configuration
    Section rxStreamFormat;   ///< RX stream format configuration
    Section extSync;          ///< External sync status
    Section reserved;         ///< Reserved section
    
    /// Total wire size for section descriptors (5 sections × 8 bytes)
    static constexpr size_t kWireSize = 5 * Section::kWireSize;
    
    /// Parse all sections from big-endian wire data
    static GeneralSections Deserialize(const uint8_t* data) {
        GeneralSections s;
        s.global         = Section::Deserialize(data);
        s.txStreamFormat = Section::Deserialize(data + 8);
        s.rxStreamFormat = Section::Deserialize(data + 16);
        s.extSync        = Section::Deserialize(data + 24);
        s.reserved       = Section::Deserialize(data + 32);
        return s;
    }

    static GeneralSections FromWire(const uint8_t* data) {
        return Deserialize(data);
    }
};

// ============================================================================
// TCAT Extension Sections
// ============================================================================

/// TCAT protocol extension sections.
struct ExtensionSections {
    Section caps;           ///< Capability section
    Section command;        ///< Command section
    Section mixer;          ///< Mixer section
    Section peak;           ///< Peak meter section
    Section router;         ///< Router configuration section
    Section streamFormat;   ///< Stream format section
    Section currentConfig;  ///< Current configuration section
    Section standalone;     ///< Standalone configuration section
    Section application;    ///< Vendor-specific application section

    static constexpr size_t kWireSize = 9 * Section::kWireSize;

    static ExtensionSections Deserialize(const uint8_t* data) {
        ExtensionSections s;
        s.caps          = Section::Deserialize(data);
        s.command       = Section::Deserialize(data + 8);
        s.mixer         = Section::Deserialize(data + 16);
        s.peak          = Section::Deserialize(data + 24);
        s.router        = Section::Deserialize(data + 32);
        s.streamFormat  = Section::Deserialize(data + 40);
        s.currentConfig = Section::Deserialize(data + 48);
        s.standalone    = Section::Deserialize(data + 56);
        s.application   = Section::Deserialize(data + 64);
        return s;
    }

    static ExtensionSections FromWire(const uint8_t* data) {
        return Deserialize(data);
    }
};

[[nodiscard]] constexpr uint32_t ExtensionAbsoluteOffset(const Section& section,
                                                         uint32_t offset = 0) noexcept {
    return kDICEExtensionOffset + section.offset + offset;
}

// ============================================================================
// Clock Source
// ============================================================================

/// Clock source identifiers (per DICE specification)
enum class ClockSource : uint8_t {
    AES1      = 0x00,
    AES2      = 0x01,
    AES3      = 0x02,
    AES4      = 0x03,
    AESAny    = 0x04,
    ADAT      = 0x05,
    TDIF      = 0x06,
    WordClock = 0x07,
    ARX1      = 0x08,
    ARX2      = 0x09,
    ARX3      = 0x0A,
    ARX4      = 0x0B,
    Internal  = 0x0C,
};

// ============================================================================
// Sample Rate
// ============================================================================

/// Standard sample rates
enum class SampleRate : uint32_t {
    Rate32000  = 32000,
    Rate44100  = 44100,
    Rate48000  = 48000,
    Rate88200  = 88200,
    Rate96000  = 96000,
    Rate176400 = 176400,
    Rate192000 = 192000,
};

/// Sample rate capability flags (bitmask)
namespace RateCaps {
    constexpr uint32_t k32000  = 0x01;
    constexpr uint32_t k44100  = 0x02;
    constexpr uint32_t k48000  = 0x04;
    constexpr uint32_t k88200  = 0x08;
    constexpr uint32_t k96000  = 0x10;
    constexpr uint32_t k176400 = 0x20;
    constexpr uint32_t k192000 = 0x40;
}

// ============================================================================
// Global Section State
// ============================================================================

/// Global section offsets (quadlets from section start)
namespace GlobalOffset {
    constexpr uint32_t kOwnerHi        = 0x00;
    constexpr uint32_t kOwnerLo        = 0x04;
    constexpr uint32_t kNotification   = 0x08;
    constexpr uint32_t kNickname       = 0x0C;  // 64 bytes (16 quadlets)
    constexpr uint32_t kClockSelect    = 0x4C;
    constexpr uint32_t kEnable         = 0x50;
    constexpr uint32_t kStatus         = 0x54;
    constexpr uint32_t kExtStatus      = 0x58;
    constexpr uint32_t kSampleRate     = 0x5C;
    constexpr uint32_t kVersion        = 0x60;
    constexpr uint32_t kClockCaps      = 0x64;
    constexpr uint32_t kClockSourceNames = 0x68;  // Variable length
}

namespace ClockSelect {
    constexpr uint32_t kSourceMask = 0x000000FF;
    constexpr uint32_t kRateMask   = 0x0000FF00;
    constexpr uint32_t kRateShift  = 8;
}

namespace StatusBits {
    constexpr uint32_t kSourceLocked    = 0x00000001;
    constexpr uint32_t kNominalRateMask = 0x0000FF00;
    constexpr uint32_t kNominalRateShift = 8;
}

namespace ExtStatusBits {
    constexpr uint32_t kArx1Locked = 0x00000040;
    constexpr uint32_t kArx2Locked = 0x00000080;
    constexpr uint32_t kArx3Locked = 0x00000100;
    constexpr uint32_t kArx4Locked = 0x00000200;
    constexpr uint32_t kArx1Slip   = 0x00400000;
    constexpr uint32_t kArx2Slip   = 0x00800000;
    constexpr uint32_t kArx3Slip   = 0x01000000;
    constexpr uint32_t kArx4Slip   = 0x02000000;
}

[[nodiscard]] constexpr bool IsSourceLocked(uint32_t status) noexcept {
    return (status & StatusBits::kSourceLocked) != 0;
}

[[nodiscard]] constexpr uint32_t NominalRateIndex(uint32_t status) noexcept {
    return (status & StatusBits::kNominalRateMask) >> StatusBits::kNominalRateShift;
}

[[nodiscard]] constexpr uint32_t RateHzFromIndex(uint32_t index) noexcept {
    switch (index) {
    case 0x00:
        return 32000;
    case 0x01:
        return 44100;
    case 0x02:
        return 48000;
    case 0x03:
        return 88200;
    case 0x04:
        return 96000;
    case 0x05:
        return 176400;
    case 0x06:
        return 192000;
    default:
        return 0;
    }
}

[[nodiscard]] constexpr uint32_t NominalRateHz(uint32_t status) noexcept {
    return RateHzFromIndex(NominalRateIndex(status));
}

[[nodiscard]] constexpr bool IsArx1Locked(uint32_t extStatus) noexcept {
    return (extStatus & ExtStatusBits::kArx1Locked) != 0;
}

[[nodiscard]] constexpr bool HasArx1Slip(uint32_t extStatus) noexcept {
    return (extStatus & ExtStatusBits::kArx1Slip) != 0;
}

constexpr uint64_t kOwnerNoOwner = 0xFFFF000000000000ULL;
constexpr uint32_t kOwnerNodeShift = 48;

/// TX stream section offsets (relative to TX section base)
namespace TxOffset {
    constexpr uint32_t kNumber       = 0x00;  // Number of TX streams
    constexpr uint32_t kSize         = 0x04;  // Size of each stream config (quadlets)
    constexpr uint32_t kIsochronous  = 0x08;  // Isoch channel (-1 = disabled)
    constexpr uint32_t kNumberAudio  = 0x0C;  // Number of audio channels
    constexpr uint32_t kNumberMidi   = 0x10;  // Number of MIDI ports
    constexpr uint32_t kSpeed        = 0x14;  // Transmission speed (0=S100..2=S400)
    constexpr uint32_t kNames        = 0x18;  // Channel names (256 bytes)
}

/// RX stream section offsets (relative to RX section base)
namespace RxOffset {
    constexpr uint32_t kNumber       = 0x00;  // Number of RX streams
    constexpr uint32_t kSize         = 0x04;  // Size of each stream config (quadlets)
    constexpr uint32_t kIsochronous  = 0x08;  // Isoch channel (-1 = disabled)
    constexpr uint32_t kSeqStart     = 0x0C;  // Sequence start index
    constexpr uint32_t kNumberAudio  = 0x10;  // Number of audio channels
    constexpr uint32_t kNumberMidi   = 0x14;  // Number of MIDI ports
    constexpr uint32_t kNames        = 0x18;  // Channel names (256 bytes)
}

/// Clock rate index (for CLOCK_SELECT register)
namespace ClockRateIndex {
    constexpr uint32_t k32000  = 0x00;
    constexpr uint32_t k44100  = 0x01;
    constexpr uint32_t k48000  = 0x02;
    constexpr uint32_t k88200  = 0x03;
    constexpr uint32_t k96000  = 0x04;
    constexpr uint32_t k176400 = 0x05;
    constexpr uint32_t k192000 = 0x06;
}

/// Parsed global section state
struct GlobalState {
    uint64_t owner{0};           ///< Owner node ID
    uint32_t notification{0};    ///< Notification register
    char nickname[64]{};         ///< Device nickname (null-terminated)
    uint32_t clockSelect{0};     ///< Clock selection
    bool enabled{false};         ///< Device enabled
    uint32_t status{0};          ///< Device status
    uint32_t extStatus{0};       ///< External status
    uint32_t sampleRate{0};      ///< Current sample rate (Hz)
    uint32_t version{0};         ///< DICE version
    uint32_t clockCaps{0};       ///< Clock capabilities bitmask
    
    /// Get supported sample rates as a human-readable string
    const char* SupportedRatesDescription() const;
};

// ============================================================================
// TX/RX Stream Format
// ============================================================================

/// Stream format entry (per-stream configuration).
/// A single superset layout is used for both TX and RX sections:
/// - TX uses `speed`
/// - RX uses `seqStart`
struct StreamFormatEntry {
    int32_t isoChannel{-1};        ///< Isochronous channel (-1 = disabled)
    uint32_t seqStart{0};          ///< RX-only: first quadlet index to interpret
    uint32_t pcmChannels{0};       ///< Number of PCM/audio channels
    uint32_t midiPorts{0};         ///< Number of MIDI ports
    uint32_t speed{0};             ///< TX-only IEEE1394 speed code
    bool hasSeqStart{false};       ///< True when parsed from RX stream section
    bool hasSpeed{false};          ///< True when parsed from TX stream section
    char labels[256]{};            ///< Channel labels blob (NUL-terminated if possible)

    [[nodiscard]] uint32_t Am824Slots() const noexcept {
        return pcmChannels + ((midiPorts + 7u) / 8u);
    }

    [[nodiscard]] bool IsActive() const noexcept {
        return isoChannel >= 0;
    }
};

/// TX/RX stream section configuration
struct StreamConfig {
    uint32_t numStreams{0};            ///< Number of streams in this section
    uint32_t entrySizeBytes{0};        ///< Entry size (from TCAT section header)
    uint32_t parsedEntrySizeBytes{0};  ///< Actual stride used by parser (currently same as entrySizeBytes)
    bool isRxLayout{false};            ///< Whether entries follow RX layout
    StreamFormatEntry streams[4];      ///< Up to 4 streams

    [[nodiscard]] uint32_t TotalPcmChannels() const noexcept {
        uint32_t total = 0;
        for (uint32_t i = 0; i < numStreams && i < 4; ++i) {
            total += streams[i].pcmChannels;
        }
        return total;
    }

    [[nodiscard]] uint32_t ActivePcmChannels() const noexcept {
        uint32_t total = 0;
        for (uint32_t i = 0; i < numStreams && i < 4; ++i) {
            if (streams[i].IsActive()) {
                total += streams[i].pcmChannels;
            }
        }
        return total;
    }

    [[nodiscard]] uint32_t TotalMidiPorts() const noexcept {
        uint32_t total = 0;
        for (uint32_t i = 0; i < numStreams && i < 4; ++i) {
            total += streams[i].midiPorts;
        }
        return total;
    }

    [[nodiscard]] uint32_t ActiveMidiPorts() const noexcept {
        uint32_t total = 0;
        for (uint32_t i = 0; i < numStreams && i < 4; ++i) {
            if (streams[i].IsActive()) {
                total += streams[i].midiPorts;
            }
        }
        return total;
    }

    [[nodiscard]] uint32_t TotalAm824Slots() const noexcept {
        uint32_t total = 0;
        for (uint32_t i = 0; i < numStreams && i < 4; ++i) {
            total += streams[i].Am824Slots();
        }
        return total;
    }

    [[nodiscard]] uint32_t ActiveAm824Slots() const noexcept {
        uint32_t total = 0;
        for (uint32_t i = 0; i < numStreams && i < 4; ++i) {
            if (streams[i].IsActive()) {
                total += streams[i].Am824Slots();
            }
        }
        return total;
    }

    [[nodiscard]] uint32_t ActiveStreamCount() const noexcept {
        uint32_t total = 0;
        for (uint32_t i = 0; i < numStreams && i < 4; ++i) {
            if (streams[i].IsActive()) {
                ++total;
            }
        }
        return total;
    }

    [[nodiscard]] uint8_t FirstActiveIsoChannel(uint8_t fallback) const noexcept {
        for (uint32_t i = 0; i < numStreams && i < 4; ++i) {
            if (streams[i].IsActive() && streams[i].isoChannel <= 0x3F) {
                return static_cast<uint8_t>(streams[i].isoChannel);
            }
        }
        return fallback;
    }

    [[nodiscard]] uint32_t DisabledPcmChannels() const noexcept {
        const uint32_t total = TotalPcmChannels();
        const uint32_t active = ActivePcmChannels();
        return (total > active) ? (total - active) : 0;
    }

    // Legacy alias kept while call sites migrate to explicit semantics.
    [[nodiscard]] uint32_t TotalChannels() const noexcept { return TotalPcmChannels(); }
};

// ============================================================================
// Complete Device Capabilities
// ============================================================================

/// Complete DICE device capabilities
struct DICECapabilities {
    GlobalState global;
    StreamConfig txStreams;
    StreamConfig rxStreams;
    bool valid{false};
};

// ============================================================================
// Notification Flags
// ============================================================================

/// Notification flags from DICE device
namespace Notify {
    constexpr uint32_t kRxConfigChange   = 0x00000001;
    constexpr uint32_t kTxConfigChange   = 0x00000002;
    constexpr uint32_t kLockChange       = 0x00000010;
    constexpr uint32_t kClockAccepted    = 0x00000020;
    constexpr uint32_t kExtStatus        = 0x00000040;
}

namespace ExtensionCommandOffset {
    constexpr uint32_t kOpcode = 0x0000;
    constexpr uint32_t kReturn = 0x0004;
}

namespace ExtensionCommandOpcode {
    constexpr uint32_t kExecute = 0x80000000;
    constexpr uint32_t kRateLow = 0x00010000;
    constexpr uint32_t kRateMiddle = 0x00020000;
    constexpr uint32_t kRateHigh = 0x00040000;
    constexpr uint32_t kLoadRouter = 0x00000001;
    constexpr uint32_t kLoadStreamConfig = 0x00000002;
    constexpr uint32_t kLoadRouterStreamConfig = 0x00000003;
}

namespace CurrentConfigOffset {
    constexpr uint32_t kLowRouter = 0x0000;
    constexpr uint32_t kLowStream = 0x1000;
    constexpr uint32_t kMiddleRouter = 0x2000;
    constexpr uint32_t kMiddleStream = 0x3000;
    constexpr uint32_t kHighRouter = 0x4000;
    constexpr uint32_t kHighStream = 0x5000;
}

} // namespace ASFW::Audio::DICE
