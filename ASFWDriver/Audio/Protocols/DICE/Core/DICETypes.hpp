// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// DICETypes.hpp - Core DICE protocol types
// Reference: TCAT DICE protocol, snd-firewire-ctl-services/protocols/dice/src/tcat.rs

#pragma once

#include "../../../../Async/AsyncTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>

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

// GLOBAL_EXTENDED_STATUS (0x58) bit layout. Per-source clock LOCK flags live in
// the low half; the matching SLIP flags live in the high half. Bit positions
// follow the TCAT DICE reference (cf. FFADO dice_defines.h DICE_EXT_STATUS_*).
// ARX1..ARX4 are the device's *audio receive* streams, i.e. the isoch streams it
// receives from us — ARX1 is our host->device transmit stream.
namespace ExtStatusBits {
    constexpr uint32_t kAes0Locked = 0x00000001;
    constexpr uint32_t kAes1Locked = 0x00000002;
    constexpr uint32_t kAes2Locked = 0x00000004;
    constexpr uint32_t kAes3Locked = 0x00000008;
    constexpr uint32_t kAdatLocked = 0x00000010;
    constexpr uint32_t kTdifLocked = 0x00000020;
    constexpr uint32_t kArx1Locked = 0x00000040;
    constexpr uint32_t kArx2Locked = 0x00000080;
    constexpr uint32_t kArx3Locked = 0x00000100;
    constexpr uint32_t kArx4Locked = 0x00000200;
    constexpr uint32_t kWordClockLocked = 0x00000400;

    constexpr uint32_t kAes0Slip   = 0x00010000;
    constexpr uint32_t kAes1Slip   = 0x00020000;
    constexpr uint32_t kAes2Slip   = 0x00040000;
    constexpr uint32_t kAes3Slip   = 0x00080000;
    constexpr uint32_t kAdatSlip   = 0x00100000;
    constexpr uint32_t kTdifSlip   = 0x00200000;
    constexpr uint32_t kArx1Slip   = 0x00400000;
    constexpr uint32_t kArx2Slip   = 0x00800000;
    constexpr uint32_t kArx3Slip   = 0x01000000;
    constexpr uint32_t kArx4Slip   = 0x02000000;
    constexpr uint32_t kWordClockSlip = 0x04000000;
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

// ============================================================================
// Notification Flags
// ============================================================================

/// Notification flags from DICE device (GLOBAL_NOTIFICATION, 0x08)
namespace Notify {
    constexpr uint32_t kRxConfigChange   = 0x00000001;
    constexpr uint32_t kTxConfigChange   = 0x00000002;
    constexpr uint32_t kLockChange       = 0x00000010;
    constexpr uint32_t kClockAccepted    = 0x00000020;
    constexpr uint32_t kExtStatus        = 0x00000040;
}

// ============================================================================
// Human-readable register decoders (diagnostics)
// ----------------------------------------------------------------------------
// These turn raw DICE register words into log-friendly strings so a log line
// reflects what the hardware is actually reporting instead of a hex blob. Each
// writes into a caller-provided buffer and returns it for direct use in
// ASFW_LOG(... "%{public}s" ...). Pure/host-testable; no DriverKit dependency.
// ============================================================================

namespace Detail {

struct BitName final {
    uint32_t bit;
    const char* name;
};

/// Append a NUL-terminated token at `len`, advancing `len`. Bounds-checked.
inline void AppendStr(char* buf, size_t cap, size_t& len, const char* str) noexcept {
    if (len + 1 >= cap) {
        return;
    }
    const int written = std::snprintf(buf + len, cap - len, "%s", str);
    if (written > 0) {
        len += static_cast<size_t>(written);
        if (len >= cap) {
            len = cap - 1;
        }
    }
}

/// Append the names of every set bit, '|'-separated, or "none" if none set.
inline void AppendBitList(char* buf, size_t cap, size_t& len, uint32_t value,
                          const BitName* flags, size_t count) noexcept {
    bool first = true;
    for (size_t i = 0; i < count; ++i) {
        if ((value & flags[i].bit) == 0) {
            continue;
        }
        if (!first) {
            AppendStr(buf, cap, len, "|");
        }
        AppendStr(buf, cap, len, flags[i].name);
        first = false;
    }
    if (first) {
        AppendStr(buf, cap, len, "none");
    }
}

} // namespace Detail

/// Decode GLOBAL_STATUS (0x54): clock-domain lock state + nominal rate.
inline const char* FormatGlobalStatus(uint32_t status, char* buf,
                                                    size_t cap) noexcept {
    if (!buf || cap == 0) {
        return "";
    }
    const char* lock = IsSourceLocked(status) ? "LOCKED" : "UNLOCKED";
    const uint32_t rate = NominalRateHz(status);
    if (rate != 0) {
        std::snprintf(buf, cap, "%s %uHz", lock, rate);
    } else {
        std::snprintf(buf, cap, "%s rate?(idx=%u)", lock, NominalRateIndex(status));
    }
    return buf;
}

/// Decode GLOBAL_EXTENDED_STATUS (0x58): which clock sources are locked, and
/// which are slipping. ARX1 is our host->device transmit stream.
inline const char* FormatExtStatus(uint32_t ext, char* buf,
                                                 size_t cap) noexcept {
    if (!buf || cap == 0) {
        return "";
    }
    static constexpr Detail::BitName kLock[] = {
        {ExtStatusBits::kAes0Locked, "AES0"}, {ExtStatusBits::kAes1Locked, "AES1"},
        {ExtStatusBits::kAes2Locked, "AES2"}, {ExtStatusBits::kAes3Locked, "AES3"},
        {ExtStatusBits::kAdatLocked, "ADAT"}, {ExtStatusBits::kTdifLocked, "TDIF"},
        {ExtStatusBits::kArx1Locked, "ARX1"}, {ExtStatusBits::kArx2Locked, "ARX2"},
        {ExtStatusBits::kArx3Locked, "ARX3"}, {ExtStatusBits::kArx4Locked, "ARX4"},
        {ExtStatusBits::kWordClockLocked, "WC"},
    };
    static constexpr Detail::BitName kSlip[] = {
        {ExtStatusBits::kAes0Slip, "AES0"}, {ExtStatusBits::kAes1Slip, "AES1"},
        {ExtStatusBits::kAes2Slip, "AES2"}, {ExtStatusBits::kAes3Slip, "AES3"},
        {ExtStatusBits::kAdatSlip, "ADAT"}, {ExtStatusBits::kTdifSlip, "TDIF"},
        {ExtStatusBits::kArx1Slip, "ARX1"}, {ExtStatusBits::kArx2Slip, "ARX2"},
        {ExtStatusBits::kArx3Slip, "ARX3"}, {ExtStatusBits::kArx4Slip, "ARX4"},
        {ExtStatusBits::kWordClockSlip, "WC"},
    };
    size_t len = 0;
    Detail::AppendStr(buf, cap, len, "lock[");
    Detail::AppendBitList(buf, cap, len, ext, kLock,
                          sizeof(kLock) / sizeof(kLock[0]));
    Detail::AppendStr(buf, cap, len, "] slip[");
    Detail::AppendBitList(buf, cap, len, ext, kSlip,
                          sizeof(kSlip) / sizeof(kSlip[0]));
    Detail::AppendStr(buf, cap, len, "]");
    return buf;
}

/// Decode GLOBAL_NOTIFICATION (0x08) bits the device raises. Any bit we do not
/// have a name for is appended as hex so nothing is silently dropped.
inline const char* FormatNotification(uint32_t bits, char* buf,
                                                    size_t cap) noexcept {
    if (!buf || cap == 0) {
        return "";
    }
    static constexpr Detail::BitName kFlags[] = {
        {Notify::kRxConfigChange, "RxCfgChg"},
        {Notify::kTxConfigChange, "TxCfgChg"},
        {Notify::kLockChange, "LockChg"},
        {Notify::kClockAccepted, "ClockAccepted"},
        {Notify::kExtStatus, "ExtStatus"},
    };
    uint32_t known = 0;
    for (const auto& f : kFlags) {
        known |= f.bit;
    }
    size_t len = 0;
    bool any = false;
    for (const auto& f : kFlags) {
        if ((bits & f.bit) == 0) {
            continue;
        }
        if (any) {
            Detail::AppendStr(buf, cap, len, "|");
        }
        Detail::AppendStr(buf, cap, len, f.name);
        any = true;
    }
    const uint32_t unknown = bits & ~known;
    if (unknown != 0) {
        char hex[16];
        std::snprintf(hex, sizeof(hex), "0x%x", unknown);
        if (any) {
            Detail::AppendStr(buf, cap, len, "|");
        }
        Detail::AppendStr(buf, cap, len, hex);
        any = true;
    }
    if (!any) {
        Detail::AppendStr(buf, cap, len, "none");
    }
    return buf;
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
