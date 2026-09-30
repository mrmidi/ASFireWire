// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcTypes.hpp - The AV/C frame vocabulary: command types, response codes,
// subunit addresses, company IDs and the opcodes this layer implements.
//
// Values are cross-checked against the Linux AV/C implementation
// (references/alsa-userspace-control-protocols-impl/protocols/ta1394, MIT) and
// TA 2004006 AV/C General 4.2. Fresh implementation; no reference code copied.
//
// Namespace ASFW::AVC is the rebuilt AV/C layer (docs/avc-rebuild). It lives

#pragma once

#include <array>
#include <cstdint>

namespace ASFW::AVC {

/// Command type ("ctype"): the low nibble of byte 0 of a command frame. The high
/// nibble (CTS) is 0 for AV/C. ta1394 general/src/lib.rs:231-235.
enum class CommandType : uint8_t {
    kControl = 0x0,
    kStatus = 0x1,
    kSpecificInquiry = 0x2,
    kNotify = 0x3,
    kGeneralInquiry = 0x4,
};

/// Response code: the low nibble of byte 0 of a response frame.
/// ta1394 general/src/lib.rs:292-298 (values), :458 (mask 0x0F).
enum class ResponseCode : uint8_t {
    kNotImplemented = 0x8,
    kAccepted = 0x9,
    kRejected = 0xA,
    kInTransition = 0xB,
    kImplementedStable = 0xC,
    kChanged = 0xD,
    kInterim = 0xF,
};

inline constexpr uint8_t kCtsMask = 0xF0;
inline constexpr uint8_t kCodeMask = 0x0F;

/// True for every response code that ends a transaction. INTERIM is the only
/// non-final one: the final response follows as a separate frame.
[[nodiscard]] constexpr bool IsFinal(ResponseCode code) noexcept {
    return code != ResponseCode::kInterim;
}

/// Subunit type: bits 7..3 of the address byte.
/// ta1394 general/src/lib.rs:36-49; General 4.2 Table 11 (Music is defined by
/// TA 2001007 Music Subunit 1.0, not by General 4.2). kUnit is the type half of
/// the unit address 0xFF.
enum class SubunitType : uint8_t {
    kMonitor = 0x00,
    kAudio = 0x01,
    kPrinter = 0x02,
    kDisc = 0x03,
    kTape = 0x04,
    kTuner = 0x05,
    kCa = 0x06,
    kCamera = 0x07,
    kPanel = 0x09,
    kBulletinBoard = 0x0A,
    kCameraStorage = 0x0B,
    kMusic = 0x0C,
    kVendorUnique = 0x1C,
    kExtended = 0x1E,  ///< subunit_type continues in the next byte (not modelled).
    kUnit = 0x1F,
};

/// Byte 1 of every frame: a subunit (type << 3 | id) or the unit (0xFF).
/// ta1394 general/src/lib.rs:131-165 (shift 3, type mask 0x1F, id mask 0x07),
/// :187 (unit address 0xFF).
class SubunitAddress {
public:
    [[nodiscard]] static constexpr SubunitAddress Unit() noexcept { return SubunitAddress{0xFF}; }

    [[nodiscard]] static constexpr SubunitAddress Of(SubunitType type, uint8_t id) noexcept {
        return SubunitAddress{static_cast<uint8_t>(((static_cast<uint8_t>(type) & 0x1F) << 3) |
                                                   (id & 0x07))};
    }

    [[nodiscard]] static constexpr SubunitAddress FromByte(uint8_t byte) noexcept {
        return SubunitAddress{byte};
    }

    [[nodiscard]] constexpr uint8_t Byte() const noexcept { return byte_; }
    [[nodiscard]] constexpr SubunitType Type() const noexcept {
        return static_cast<SubunitType>((byte_ >> 3) & 0x1F);
    }
    [[nodiscard]] constexpr uint8_t Id() const noexcept { return byte_ & 0x07; }
    [[nodiscard]] constexpr bool IsUnit() const noexcept { return byte_ == 0xFF; }

    /// The type continues in the next byte. This layer does not model extended
    /// addressing; codecs reject it with AvcErrorKind::kUnsupported.
    [[nodiscard]] constexpr bool IsExtendedType() const noexcept {
        return Type() == SubunitType::kExtended;
    }

    friend constexpr bool operator==(SubunitAddress, SubunitAddress) noexcept = default;

private:
    explicit constexpr SubunitAddress(uint8_t byte) noexcept : byte_(byte) {}
    uint8_t byte_;
};

inline constexpr SubunitAddress kUnitAddress = SubunitAddress::Unit();
inline constexpr SubunitAddress kAudioSubunit0 = SubunitAddress::Of(SubunitType::kAudio, 0);
inline constexpr SubunitAddress kMusicSubunit0 = SubunitAddress::Of(SubunitType::kMusic, 0);

// Linux addresses the audio subunit as 0x08 (bebob_command.c:21) and the music
// subunit as 0x60 (bebob.h:189).
static_assert(kUnitAddress.Byte() == 0xFF);
static_assert(kAudioSubunit0.Byte() == 0x08);
static_assert(kMusicSubunit0.Byte() == 0x60);
static_assert(kMusicSubunit0.Type() == SubunitType::kMusic && kMusicSubunit0.Id() == 0);

/// Opcodes implemented by the AV/C command codecs. Other command families add their own
/// (descriptors, connections). Spec names live in the comments.
enum class Opcode : uint8_t {
    kVendorDependent = 0x00,         ///< VENDOR-DEPENDENT; ta1394 general.rs:226
    kPlugInfo = 0x02,                ///< PLUG INFO; ta1394 general.rs:378
    kOpenDescriptor = 0x08,          ///< OPEN DESCRIPTOR; TA 2002013 §7.1
    kReadDescriptor = 0x09,          ///< READ DESCRIPTOR; TA 2002013 §7.5
    kOutputPlugSignalFormat = 0x18,  ///< OUTPUT PLUG SIGNAL FORMAT; General 4.2 §10.11, general.rs:560
    kInputPlugSignalFormat = 0x19,   ///< INPUT PLUG SIGNAL FORMAT; General 4.2 §10.10, general.rs:521
    kSignalSource = 0x1A,            ///< SIGNAL SOURCE (CCM); ta1394 ccm/src/lib.rs:213
    kStreamFormatSupport = 0x2F,     ///< STREAM FORMAT SUPPORT; TA 2001002 (BridgeCo, Linux bebob_command.c:302)
    kUnitInfo = 0x30,                ///< UNIT INFO; ta1394 general.rs:37
    kSubunitInfo = 0x31,             ///< SUBUNIT INFO; ta1394 general.rs:130
    kFunctionBlock = 0xB8,           ///< FUNCTION BLOCK (audio subunit); ta1394 audio/src/lib.rs:256
    kExtendedStreamFormat = 0xBF,    ///< EXTENDED STREAM FORMAT INFORMATION (unpublished draft);
                                     ///< Linux oxfw-command.c:22, ta1394 stream-format lib.rs:1124
};

/// IEEE company ID (OUI), as carried in UNIT INFO and VENDOR-DEPENDENT frames,
/// most significant byte first.
using CompanyId = std::array<uint8_t, 3>;

[[nodiscard]] constexpr uint32_t ToOui(const CompanyId& id) noexcept {
    return (static_cast<uint32_t>(id[0]) << 16) | (static_cast<uint32_t>(id[1]) << 8) | id[2];
}

[[nodiscard]] constexpr CompanyId CompanyIdFromOui(uint32_t oui) noexcept {
    return CompanyId{static_cast<uint8_t>(oui >> 16), static_cast<uint8_t>(oui >> 8),
                     static_cast<uint8_t>(oui)};
}

static_assert(ToOui(CompanyIdFromOui(0x001198)) == 0x001198);

// ---------------------------------------------------------------------------
// AvcVolume - Typed volume in 1/256 dB (TA 1999008 §10.3.2, ta1394 audio:355)
// 0x7FFF is invalid; 0x8000 is -infinity dB (full mute / cutoff).
// ---------------------------------------------------------------------------

struct AvcVolume {
    int16_t raw{kInvalidRaw};

    static constexpr int16_t kInvalidRaw = 0x7FFF;
    static constexpr int16_t kNegativeInfinityRaw = static_cast<int16_t>(0x8000);

    static constexpr AvcVolume Invalid() noexcept { return AvcVolume{kInvalidRaw}; }
    static constexpr AvcVolume NegativeInfinity() noexcept { return AvcVolume{kNegativeInfinityRaw}; }
    static constexpr AvcVolume FromRaw(int16_t r) noexcept { return AvcVolume{r}; }
    static constexpr AvcVolume FromDb(float db) noexcept {
        if (db <= -128.0f) {
            return NegativeInfinity();
        }
        float val = db * 256.0f;
        if (val > 32766.0f) val = 32766.0f;
        if (val < -32767.0f) val = -32767.0f;
        return AvcVolume{static_cast<int16_t>(val)};
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept { return raw != kInvalidRaw; }
    [[nodiscard]] constexpr bool IsNegativeInfinity() const noexcept { return raw == kNegativeInfinityRaw; }
    [[nodiscard]] constexpr float ToDb() const noexcept {
        if (IsNegativeInfinity()) {
            return -1000.0f; // Represents -infinity in float
        }
        return static_cast<float>(raw) / 256.0f;
    }
    [[nodiscard]] constexpr int16_t Raw() const noexcept { return raw; }

    friend constexpr bool operator==(AvcVolume, AvcVolume) noexcept = default;
};

static_assert(AvcVolume::Invalid().raw == 0x7FFF);
static_assert(AvcVolume::NegativeInfinity().raw == static_cast<int16_t>(0x8000));
static_assert(AvcVolume::FromRaw(0x0000).ToDb() == 0.0f);
static_assert(AvcVolume::FromRaw(0x0100).ToDb() == 1.0f);
static_assert(AvcVolume::FromRaw(static_cast<int16_t>(0xFF00)).ToDb() == -1.0f);

} // namespace ASFW::AVC
