// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcNames.hpp - Spec names for the AV/C frame vocabulary, for logs and diagnostics.
//
// Contract:
// - Every value a device sends is described as `name(0xNN)`. A value the table does not know is
//   described as `UNKNOWN(<table>:0xNN)`, never dropped and never guessed. Reading a log, an
//   UNKNOWN is a value nobody has named yet.
// - A table holds the enumerators themselves, so a value is defined once (in its enum) and named
//   once (here). A new enumerator without a table entry logs as UNKNOWN until it is named.
// - The names are the spec's own words (TA 2004006 Tables 8, 9, 11; the opcode names in each
//   command's section).

#pragma once

#include "AvcError.hpp"
#include "AvcTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ASFW::AVC {

struct NameEntry {
    uint32_t value;
    std::string_view name;
};

[[nodiscard]] constexpr std::optional<std::string_view> LookupName(std::span<const NameEntry> table,
                                                                  uint32_t value) noexcept {
    for (const auto& entry : table) {
        if (entry.value == value) return entry.name;
    }
    return std::nullopt;
}

/// Lower-case hex of `value`, at least `digits` digits, with a 0x prefix.
[[nodiscard]] inline std::string Hex(uint32_t value, unsigned digits = 2) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    do {
        text.insert(text.begin(), kDigits[value & 0xF]);
        value >>= 4;
    } while (value != 0);
    while (text.size() < digits) text.insert(text.begin(), '0');
    return "0x" + text;
}

/// A 64-bit value (a GUID) as 16 hex digits with a 0x prefix.
[[nodiscard]] inline std::string Hex64(uint64_t value) {
    return "0x" + Hex(static_cast<uint32_t>(value >> 32), 8).substr(2) + Hex(static_cast<uint32_t>(value), 8).substr(2);
}

/// Bytes as space-separated lower-case hex pairs: "01 ff 1a".
[[nodiscard]] inline std::string HexBytes(std::span<const uint8_t> bytes) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    for (const uint8_t b : bytes) {
        if (!text.empty()) text.push_back(' ');
        text.push_back(kDigits[b >> 4]);
        text.push_back(kDigits[b & 0xF]);
    }
    return text;
}

/// "name(0xNN)" for a value the table knows, "UNKNOWN(<table>:0xNN)" for one it does not.
[[nodiscard]] inline std::string DescribeValue(std::span<const NameEntry> table, std::string_view tableName,
                                               uint32_t value, unsigned hexDigits = 2) {
    if (const auto name = LookupName(table, value)) {
        return std::string(*name) + "(" + Hex(value, hexDigits) + ")";
    }
    return "UNKNOWN(" + std::string(tableName) + ":" + Hex(value, hexDigits) + ")";
}

namespace names {

// TA 2004006 §5.3.1 Table 8.
inline constexpr std::array kCommandTypes{
    NameEntry{static_cast<uint32_t>(CommandType::kControl), "CONTROL"},
    NameEntry{static_cast<uint32_t>(CommandType::kStatus), "STATUS"},
    NameEntry{static_cast<uint32_t>(CommandType::kSpecificInquiry), "SPECIFIC INQUIRY"},
    NameEntry{static_cast<uint32_t>(CommandType::kNotify), "NOTIFY"},
    NameEntry{static_cast<uint32_t>(CommandType::kGeneralInquiry), "GENERAL INQUIRY"},
};

// TA 2004006 §5.3.2 Table 9.
inline constexpr std::array kResponseCodes{
    NameEntry{static_cast<uint32_t>(ResponseCode::kNotImplemented), "NOT IMPLEMENTED"},
    NameEntry{static_cast<uint32_t>(ResponseCode::kAccepted), "ACCEPTED"},
    NameEntry{static_cast<uint32_t>(ResponseCode::kRejected), "REJECTED"},
    NameEntry{static_cast<uint32_t>(ResponseCode::kInTransition), "IN TRANSITION"},
    NameEntry{static_cast<uint32_t>(ResponseCode::kImplementedStable), "IMPLEMENTED/STABLE"},
    NameEntry{static_cast<uint32_t>(ResponseCode::kChanged), "CHANGED"},
    NameEntry{static_cast<uint32_t>(ResponseCode::kInterim), "INTERIM"},
};

// TA 2004006 §5.3.4.1 Table 11; Music is defined by TA 2001007.
inline constexpr std::array kSubunitTypes{
    NameEntry{static_cast<uint32_t>(SubunitType::kMonitor), "Monitor"},
    NameEntry{static_cast<uint32_t>(SubunitType::kAudio), "Audio"},
    NameEntry{static_cast<uint32_t>(SubunitType::kPrinter), "Printer"},
    NameEntry{static_cast<uint32_t>(SubunitType::kDisc), "Disc"},
    NameEntry{static_cast<uint32_t>(SubunitType::kTape), "Tape recorder/player"},
    NameEntry{static_cast<uint32_t>(SubunitType::kTuner), "Tuner"},
    NameEntry{static_cast<uint32_t>(SubunitType::kCa), "CA"},
    NameEntry{static_cast<uint32_t>(SubunitType::kCamera), "Camera"},
    NameEntry{static_cast<uint32_t>(SubunitType::kPanel), "Panel"},
    NameEntry{static_cast<uint32_t>(SubunitType::kBulletinBoard), "Bulletin Board"},
    NameEntry{static_cast<uint32_t>(SubunitType::kCameraStorage), "Camera Storage"},
    NameEntry{static_cast<uint32_t>(SubunitType::kMusic), "Music"},
    NameEntry{static_cast<uint32_t>(SubunitType::kVendorUnique), "Vendor unique"},
    NameEntry{static_cast<uint32_t>(SubunitType::kExtended), "Extended"},
    NameEntry{static_cast<uint32_t>(SubunitType::kUnit), "Unit"},
};

// The opcode names are the spec's: TA 2004006 §9-§10, TA 2002010 Table 7.1, TA 2002013 §7, TA 2001002,
// TA 1999008 §10. These are the opcodes whose meaning does not depend on the subunit type. A subunit type
// defines its own (below); the same number means different commands in different subunit types. 0xBF is the unpublished extended stream format draft.
inline constexpr std::array kOpcodes{
    NameEntry{static_cast<uint32_t>(Opcode::kVendorDependent), "VENDOR-DEPENDENT"},
    NameEntry{static_cast<uint32_t>(Opcode::kPlugInfo), "PLUG INFO"},
    NameEntry{static_cast<uint32_t>(Opcode::kOpenDescriptor), "OPEN DESCRIPTOR"},
    NameEntry{static_cast<uint32_t>(Opcode::kReadDescriptor), "READ DESCRIPTOR"},
    NameEntry{static_cast<uint32_t>(Opcode::kOutputPlugSignalFormat), "OUTPUT PLUG SIGNAL FORMAT"},
    NameEntry{static_cast<uint32_t>(Opcode::kInputPlugSignalFormat), "INPUT PLUG SIGNAL FORMAT"},
    NameEntry{static_cast<uint32_t>(Opcode::kSignalSource), "SIGNAL SOURCE"},
    NameEntry{static_cast<uint32_t>(Opcode::kInputSelect), "INPUT SELECT"},
    NameEntry{static_cast<uint32_t>(Opcode::kOutputPreset), "OUTPUT PRESET"},
    NameEntry{static_cast<uint32_t>(Opcode::kCcmProfile), "CCM PROFILE"},
    NameEntry{static_cast<uint32_t>(Opcode::kStreamFormatSupport), "STREAM FORMAT SUPPORT"},
    NameEntry{static_cast<uint32_t>(Opcode::kUnitInfo), "UNIT INFO"},
    NameEntry{static_cast<uint32_t>(Opcode::kSubunitInfo), "SUBUNIT INFO"},
    NameEntry{static_cast<uint32_t>(Opcode::kFunctionBlock), "FUNCTION BLOCK"},
    NameEntry{static_cast<uint32_t>(Opcode::kExtendedStreamFormat), "EXTENDED STREAM FORMAT INFORMATION"},
};

// TA 2001007 Table 7.1: the opcodes of the Music subunit.
inline constexpr std::array kMusicOpcodes{
    NameEntry{static_cast<uint32_t>(Opcode::kDestinationPlugConfigure), "DESTINATION PLUG CONFIGURE"},
    NameEntry{static_cast<uint32_t>(Opcode::kSourcePlugConfigure), "SOURCE PLUG CONFIGURE"},
    NameEntry{static_cast<uint32_t>(Opcode::kDestinationConfigurations), "DESTINATION CONFIGURATIONS"},
    NameEntry{static_cast<uint32_t>(Opcode::kSourceConfigurations), "SOURCE CONFIGURATIONS"},
    NameEntry{static_cast<uint32_t>(Opcode::kMusicPlugInfo), "MUSIC PLUG INFO"},
    NameEntry{static_cast<uint32_t>(Opcode::kCurrentCapability), "CURRENT CAPABILITY"},
};

// TA 1999008 §11.1: the subunit-specific opcode of the Audio subunit (FUNCTION BLOCK is common, in kOpcodes).
inline constexpr std::array kAudioOpcodes{
    NameEntry{static_cast<uint32_t>(Opcode::kChangeConfiguration), "CHANGE CONFIGURATION"},
};

// What went wrong in a codec or transaction (AvcError.hpp).
inline constexpr std::array kErrorKinds{
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kFrameTooShort), "frame too short"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kFrameTooLong), "frame too long"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kNotAResponse), "not a response"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kAddressMismatch), "address mismatch"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kOpcodeMismatch), "opcode mismatch"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kUnexpectedResponse), "unexpected response"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kOperandsTooShort), "operands too short"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kMalformedOperands), "malformed operands"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kInvalidArgument), "invalid argument"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kUnsupported), "unsupported"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kTimeout), "timeout"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kBusReset), "bus reset"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kTransportError), "transport error"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kRefused), "refused by command filter"},
    NameEntry{static_cast<uint32_t>(AvcErrorKind::kBusy), "busy"},
};

} // namespace names

[[nodiscard]] inline std::string Describe(CommandType value) {
    return DescribeValue(names::kCommandTypes, "ctype", static_cast<uint32_t>(value), 1);
}
[[nodiscard]] inline std::string Describe(ResponseCode value) {
    return DescribeValue(names::kResponseCodes, "response", static_cast<uint32_t>(value), 1);
}
[[nodiscard]] inline std::string Describe(SubunitType value) {
    return DescribeValue(names::kSubunitTypes, "subunit_type", static_cast<uint32_t>(value), 2);
}
/// The name of an opcode that does not depend on the subunit type. A subunit-specific opcode (CHANGE
/// CONFIGURATION, MUSIC PLUG INFO, ...) prints as UNKNOWN here: name it with the overload below.
[[nodiscard]] inline std::string Describe(Opcode value) {
    return DescribeValue(names::kOpcodes, "opcode", static_cast<uint32_t>(value), 2);
}
/// The name of an opcode in a command addressed to a subunit of `subunit`: the subunit type's own opcodes
/// first (Music, Audio), then the common ones.
[[nodiscard]] inline std::string Describe(SubunitType subunit, Opcode value) {
    const auto raw = static_cast<uint32_t>(value);
    if (subunit == SubunitType::kMusic) {
        if (const auto name = LookupName(names::kMusicOpcodes, raw)) return std::string(*name) + "(" + Hex(raw, 2) + ")";
    } else if (subunit == SubunitType::kAudio) {
        if (const auto name = LookupName(names::kAudioOpcodes, raw)) return std::string(*name) + "(" + Hex(raw, 2) + ")";
    }
    return Describe(value);
}
/// The same for a frame's address byte (subunit_type, subunit_ID) and opcode byte.
[[nodiscard]] inline std::string DescribeOpcodeOf(uint8_t addressByte, uint8_t opcodeByte) {
    const auto address = SubunitAddress::FromByte(addressByte);
    return Describe(address.Type(), static_cast<Opcode>(opcodeByte));
}
[[nodiscard]] inline std::string Describe(AvcErrorKind value) {
    return DescribeValue(names::kErrorKinds, "error", static_cast<uint32_t>(value), 2);
}

/// "Music(0x0c)#0 (0x60)", "Unit(0x1f) (0xff)", or "UNKNOWN(subunit_type:0x0d)#2 (0x68)" for an unnamed type.
[[nodiscard]] inline std::string Describe(SubunitAddress address) {
    if (address.IsUnit()) return Describe(SubunitType::kUnit) + " (" + Hex(address.Byte()) + ")";
    return Describe(address.Type()) + "#" + std::to_string(address.Id()) + " (" + Hex(address.Byte()) + ")";
}

} // namespace ASFW::AVC
