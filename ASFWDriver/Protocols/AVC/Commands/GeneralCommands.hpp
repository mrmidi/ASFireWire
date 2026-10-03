// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// GeneralCommands.hpp - AV/C General command operands: UNIT INFO, SUBUNIT INFO,
// PLUG INFO, INPUT/OUTPUT PLUG SIGNAL FORMAT, VENDOR-DEPENDENT.
//
// Layouts: Linux ta1394 general/src/general.rs (MIT); TA 2004006 AV/C General 4.2.
//

#pragma once

#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"
#include "../Core/RateCodes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ASFW::AVC::Cmd {

// ---------------------------------------------------------------------------
// UNIT INFO (0x30)
// Command operands: 07 FF FF FF FF. Response operands: [0]=07, [1]=unit
// type<<3|id, [2..4]=company ID. Linux ta1394 general.rs:41-49 sends this form;
// Apple IOFireWireAVCUnit.cpp:946-950 also sends five operands (all FF). The
// Duet and the Phase 88 were both captured answering it. Never send it bare
// (no operands): the Phase 88 does not answer, stops acking, then resets the bus.
// ---------------------------------------------------------------------------

/// operand[0] of a UNIT INFO response (TA 2004006 Figure 27) and of the command we send.
/// DEVIATION, recorded in documentation/avc-rebuild/magic-numbers-audit.md: the spec (Figure 26)
/// and Apple (IOFireWireAVCUnit.cpp:946) send FF here, ta1394 (general.rs:29) sends 07 as we do.
/// Both the Duet and the Phase 88 answer this form; do not change it without a capture.
inline constexpr uint8_t kUnitInfoFirstOperand = 0x07;

/// The "unit" field of a UNIT INFO command is 7 (TA 2004006 Table 28).
inline constexpr uint8_t kUnitInfoUnitField = 0x07;

/// company_ID of a UNIT INFO command: all FF (TA 2004006 Table 28).
inline constexpr CompanyId kUnspecifiedCompanyId{kUnspecifiedOperand, kUnspecifiedOperand, kUnspecifiedOperand};

struct UnitInfo {
    SubunitType unitType{SubunitType::kUnit};
    uint8_t unitId{kUnitInfoUnitField};
    CompanyId companyId{kUnspecifiedCompanyId};

    friend constexpr bool operator==(const UnitInfo&, const UnitInfo&) noexcept = default;
};

struct UnitInfoOperands {
    static constexpr Opcode kOpcode = Opcode::kUnitInfo;
    static constexpr bool kRequiresUnitAddress = true;

    using Reply = UnitInfo;

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t != CommandType::kStatus) {
            return Fail(AvcErrorKind::kInvalidArgument);
        }
        constexpr std::array<uint8_t, 5> kOperands = {kUnitInfoFirstOperand, kUnspecifiedOperand,
                                                      kUnspecifiedOperand, kUnspecifiedOperand,
                                                      kUnspecifiedOperand};
        return w.Append(kOperands);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < 5) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        if (in[0] != kUnitInfoFirstOperand) {
            return FailAt(AvcErrorKind::kMalformedOperands, 0);
        }

        // operand[1] is unit_type | unit, laid out like an address byte (Figure 27).
        return UnitInfo{
            .unitType = SubunitAddress::FromByte(in[1]).Type(),
            .unitId = SubunitAddress::FromByte(in[1]).Id(),
            .companyId = CompanyId{in[2], in[3], in[4]},
        };
    }
};

using UnitInfoCommand = Command<UnitInfoOperands>;

// ---------------------------------------------------------------------------
// SUBUNIT INFO (0x31)
// Command operands: [page<<4 | extension code] FF FF FF FF.
// Response operands: [0] echoes page/extension code; [1..4] up to four entries,
// each type<<3 | max id, 0xFF = empty. ta1394 general.rs `SubunitInfo`.
// ---------------------------------------------------------------------------

/// extension_code "shall presently have a value of 7" (TA 2004006 §9.3.1.1).
inline constexpr uint8_t kSubunitInfoExtensionCode = 0x07;
/// operand[0] is 0 | page (3 bits) | 0 | extension_code (3 bits) (Figure 28).
inline constexpr unsigned kSubunitInfoPageShift = 4;
inline constexpr uint8_t kSubunitInfoPageMask = 0x07;
inline constexpr uint8_t kSubunitInfoExtensionMask = 0x07;
/// A page holds at most four subunit entries (§9.3.1.1); unused slots are FF (§9.3.1.2).
inline constexpr size_t kSubunitInfoEntriesPerPage = 4;
inline constexpr uint8_t kSubunitInfoEmptyEntry = kUnspecifiedOperand;

struct SubunitInfoEntry {
    SubunitType type{SubunitType::kUnit};
    uint8_t maximumId{0};
};

struct SubunitInfo {
    uint8_t page{0};
    uint8_t extensionCode{kSubunitInfoExtensionCode};
    std::array<SubunitInfoEntry, kSubunitInfoEntriesPerPage> entries{};
    uint8_t entryCount{0};  ///< Entries before the first empty slot.
};

struct SubunitInfoOperands {
    static constexpr Opcode kOpcode = Opcode::kSubunitInfo;
    static constexpr bool kRequiresUnitAddress = true;

    uint8_t page{0};
    uint8_t extensionCode{kSubunitInfoExtensionCode};

    using Reply = SubunitInfo;

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t != CommandType::kStatus) {
            return Fail(AvcErrorKind::kInvalidArgument);
        }
        const std::array<uint8_t, 5> ops = {
            static_cast<uint8_t>(((page & kSubunitInfoPageMask) << kSubunitInfoPageShift) |
                                 (extensionCode & kSubunitInfoExtensionMask)),
            kUnspecifiedOperand, kUnspecifiedOperand, kUnspecifiedOperand, kUnspecifiedOperand  // page_data
        };
        return w.Append(ops);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < 5) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }

        SubunitInfo info{
            .page = static_cast<uint8_t>((in[0] >> kSubunitInfoPageShift) & kSubunitInfoPageMask),
            .extensionCode = static_cast<uint8_t>(in[0] & kSubunitInfoExtensionMask),
            .entries = {},
            .entryCount = 0,
        };

        for (size_t i = 0; i < kSubunitInfoEntriesPerPage; ++i) {
            const uint8_t entryByte = in[1 + i];
            if (entryByte == kSubunitInfoEmptyEntry) {
                break;
            }
            // Figure 30: subunit_type | max_subunit_ID, laid out like an address byte.
            info.entries[info.entryCount++] = SubunitInfoEntry{
                .type = SubunitAddress::FromByte(entryByte).Type(),
                .maximumId = SubunitAddress::FromByte(entryByte).Id(),
            };
        }

        return info;
    }
};

using SubunitInfoCommand = Command<SubunitInfoOperands>;

// ---------------------------------------------------------------------------
// PLUG INFO (0x02)
// Command operands: [subfunction] FF FF FF FF.
//   Unit, subfunction 0x00: [1]=iso in, [2]=iso out, [3]=external in, [4]=external out.
//   Unit, subfunction 0x01: [1]=async in, [2]=async out.
//   Subunit, subfunction 0x00: [1]=destination plugs, [2]=source plugs.
// ---------------------------------------------------------------------------

/// Counts returned by unit PLUG INFO subfunction 0x00. The unit model may own this value.
struct UnitPlugCounts {
    uint8_t isochronousInputs{0};
    uint8_t isochronousOutputs{0};
    uint8_t externalInputs{0};
    uint8_t externalOutputs{0};

    friend constexpr bool operator==(const UnitPlugCounts&, const UnitPlugCounts&) noexcept = default;
};

/// Counts returned by unit PLUG INFO subfunction 0x01.
struct UnitAsyncPlugCounts {
    uint8_t asynchronousInputs{0};
    uint8_t asynchronousOutputs{0};

    friend constexpr bool operator==(const UnitAsyncPlugCounts&, const UnitAsyncPlugCounts&) noexcept = default;
};

/// Counts returned by subunit PLUG INFO subfunction 0x00.
struct SubunitPlugCounts {
    uint8_t destinationPlugs{0};
    uint8_t sourcePlugs{0};

    friend constexpr bool operator==(const SubunitPlugCounts&, const SubunitPlugCounts&) noexcept = default;
};

/// PLUG INFO subfunction (TA 2004006 §10.1.1.1, Table 39): 00 = serial bus isochronous and
/// external plugs, 01 = serial bus asynchronous plugs. A subunit always uses 00.
inline constexpr uint8_t kPlugInfoSubfunctionIsoExternal = 0x00;
inline constexpr uint8_t kPlugInfoSubfunctionAsync = 0x01;
inline constexpr uint8_t kPlugInfoSubfunctionSubunit = 0x00;

enum class PlugInfoForm : uint8_t {
    kUnitIsoExternal,
    kUnitAsync,
    kSubunit,
};

struct PlugInfoReply {
    PlugInfoForm form{PlugInfoForm::kUnitIsoExternal};
    UnitPlugCounts unit{};
    UnitAsyncPlugCounts asynchronous{};
    SubunitPlugCounts subunit{};
};

struct PlugInfoOperands {
    static constexpr Opcode kOpcode = Opcode::kPlugInfo;

    PlugInfoForm form{PlugInfoForm::kUnitIsoExternal};
    uint8_t dummyByte{kUnspecifiedOperand};  ///< operand[1..4]: "all FF16" (Figure 36)
    using Reply = PlugInfoReply;

    [[nodiscard]] static constexpr uint8_t SubfunctionOf(PlugInfoForm form) noexcept {
        switch (form) {
            case PlugInfoForm::kUnitAsync: return kPlugInfoSubfunctionAsync;
            case PlugInfoForm::kSubunit: return kPlugInfoSubfunctionSubunit;
            case PlugInfoForm::kUnitIsoExternal: return kPlugInfoSubfunctionIsoExternal;
        }
        return kPlugInfoSubfunctionIsoExternal;
    }

    [[nodiscard]] constexpr Expected<void> ValidateAddress(SubunitAddress address) const noexcept {
        if ((form == PlugInfoForm::kSubunit) == address.IsUnit()) {
            return Fail(AvcErrorKind::kInvalidArgument);
        }
        return {};
    }

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t != CommandType::kStatus) {
            return Fail(AvcErrorKind::kInvalidArgument);
        }
        const uint8_t subfunction = SubfunctionOf(form);
        const std::array<uint8_t, 5> ops = {subfunction, dummyByte, dummyByte, dummyByte, dummyByte};
        return w.Append(ops);
    }

    [[nodiscard]] Expected<Reply> Read(std::span<const uint8_t> in) const noexcept {
        const uint8_t subfunction = SubfunctionOf(form);
        const size_t minimum = form == PlugInfoForm::kUnitIsoExternal ? 5 : 3;
        if (in.size() < minimum) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        if (in[0] != subfunction) {
            return FailAt(AvcErrorKind::kMalformedOperands, 0);
        }
        Reply reply{.form = form};
        switch (form) {
            case PlugInfoForm::kUnitIsoExternal:
                reply.unit = UnitPlugCounts{in[1], in[2], in[3], in[4]};
                break;
            case PlugInfoForm::kUnitAsync:
                reply.asynchronous = UnitAsyncPlugCounts{in[1], in[2]};
                break;
            case PlugInfoForm::kSubunit:
                reply.subunit = SubunitPlugCounts{in[1], in[2]};
                break;
        }
        return reply;
    }
};

using PlugInfoCommand = Command<PlugInfoOperands>;

// ---------------------------------------------------------------------------
// INPUT (0x19) / OUTPUT (0x18) PLUG SIGNAL FORMAT. Unit address only.
// Operands: [plug id][FMT][FDF 3 bytes].
//   STATUS: sends wildcard query [plug id][fmt][FF][FF][FF]
//   CONTROL: sends requested format
// ---------------------------------------------------------------------------

enum class PlugSignalDirection : uint8_t {
    kInput,   ///< opcode 0x19
    kOutput,  ///< opcode 0x18
};

enum class SignalFormatQuery : uint8_t {
    kAllWildcard,    ///< FF FF FF FF
    kAm824Wildcard,  ///< 90 FF FF FF
};

/// The fmt byte of the signal format commands is eoh (bit 7) | form (bit 6) | FMT (bits 5..0)
/// (TA 2004006 Figures 56, 57; Tables 64, 65). eoh is always 1 and form 0 in a control command; a
/// STATUS command asks with eoh 1, form 1 and FMT 3F, which makes the byte FF.
inline constexpr uint8_t kFmtEohBit = 0x80;
inline constexpr uint8_t kFmtFormBit = 0x40;
inline constexpr uint8_t kFmtCodeMask = 0x3F;
/// FMT of audio and music streams is 0x10 (IEC 61883-6 Table 2).
inline constexpr uint8_t kFmtAudioMusic = 0x10;
inline constexpr uint8_t kFmtAm824 = kFmtEohBit | kFmtAudioMusic;  // 0x90
/// "fmt = 3F, fdf = all FF" in a STATUS command (Table 65).
inline constexpr uint8_t kFmtStatusWildcard = kFmtEohBit | kFmtFormBit | kFmtCodeMask;  // 0xFF
inline constexpr uint8_t kFdfStatusWildcard = kUnspecifiedOperand;
/// FDF of an AM824 stream: EVT 0 in bits 5..4, SFC in bits 2..0 (IEC 61883-6 Figure 30, Tables 16, 19).
inline constexpr uint8_t kFdfSfcMask = 0x07;

static_assert(kFmtAm824 == 0x90);
static_assert(kFmtStatusWildcard == 0xFF);

struct PlugSignalFormat {
    uint8_t plugId{0};
    uint8_t fmt{kFmtStatusWildcard};
    std::array<uint8_t, 3> fdf{kFdfStatusWildcard, kFdfStatusWildcard, kFdfStatusWildcard};
};

[[nodiscard]] constexpr PlugSignalFormat Am824SignalFormat(uint8_t plugId, CipSfc sfc) noexcept {
    return PlugSignalFormat{plugId, kFmtAm824,
                            {static_cast<uint8_t>(static_cast<uint8_t>(sfc) & kFdfSfcMask),
                             kFdfStatusWildcard, kFdfStatusWildcard}};
}

[[nodiscard]] constexpr std::optional<CipSfc> SfcOf(const PlugSignalFormat& format) noexcept {
    if (format.fmt != kFmtAm824) {
        return std::nullopt;
    }
    return static_cast<CipSfc>(format.fdf[0] & kFdfSfcMask);
}

struct PlugSignalFormatOperands {
    static constexpr Opcode kOpcode = Opcode::kInputPlugSignalFormat;
    static constexpr bool kRequiresUnitAddress = true;

    PlugSignalDirection direction{PlugSignalDirection::kInput};
    uint8_t plugId{0};
    std::optional<PlugSignalFormat> format{std::nullopt};
    SignalFormatQuery query{SignalFormatQuery::kAllWildcard};

    using Reply = PlugSignalFormat;

    [[nodiscard]] constexpr Opcode GetOpcode() const noexcept {
        return (direction == PlugSignalDirection::kInput)
            ? Opcode::kInputPlugSignalFormat
            : Opcode::kOutputPlugSignalFormat;
    }

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t == CommandType::kControl || t == CommandType::kSpecificInquiry) {
            if (!format.has_value()) {
                return Fail(AvcErrorKind::kInvalidArgument);
            }
            const std::array<uint8_t, 5> ops = {
                format->plugId,
                format->fmt,
                format->fdf[0],
                format->fdf[1],
                format->fdf[2]
            };
            return w.Append(ops);
        }
        if (t == CommandType::kStatus) {
            const std::array<uint8_t, 5> ops = {
                plugId,
                (query == SignalFormatQuery::kAm824Wildcard) ? kFmtAm824 : kFmtStatusWildcard,
                kFdfStatusWildcard, kFdfStatusWildcard, kFdfStatusWildcard
            };
            return w.Append(ops);
        }
        return Fail(AvcErrorKind::kInvalidArgument);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < 5) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        return PlugSignalFormat{
            .plugId = in[0],
            .fmt = in[1],
            .fdf = {in[2], in[3], in[4]},
        };
    }
};

using PlugSignalFormatCommand = Command<PlugSignalFormatOperands>;

// ---------------------------------------------------------------------------
// VENDOR-DEPENDENT (0x00)
// ---------------------------------------------------------------------------

/// A VENDOR-DEPENDENT response retains the company ID as well as its payload.
/// Callers that care about vendor identity must validate the received ID; they
/// must not substitute the request's company ID during reply decoding.
struct RawVendorDependentReply {
    CompanyId companyId{kUnspecifiedCompanyId};
    std::vector<uint8_t> payload;
};

/// Owned VENDOR-DEPENDENT operands; the company ID and payload are supplied by the caller.
struct RawVendorDependentOperands {
    static constexpr Opcode kOpcode = Opcode::kVendorDependent;

    CompanyId companyId{kUnspecifiedCompanyId};
    std::array<uint8_t, 256> payloadBytes{};
    uint16_t payloadLength{0};

    using Reply = RawVendorDependentReply;

    RawVendorDependentOperands() = default;
    RawVendorDependentOperands(CompanyId cid, std::span<const uint8_t> data) noexcept
        : companyId(cid) {
        payloadLength = static_cast<uint16_t>(std::min(data.size(), payloadBytes.size()));
        std::copy_n(data.begin(), payloadLength, payloadBytes.begin());
    }

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType /*t*/) const noexcept {
        auto res = w.Append(companyId);
        if (!res) return res;
        return w.Append(std::span<const uint8_t>{payloadBytes.data(), payloadLength});
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < 3) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        return Reply{
            .companyId = {in[0], in[1], in[2]},
            .payload = std::vector<uint8_t>(in.begin() + 3, in.end()),
        };
    }
};

using RawVendorDependentCommand = Command<RawVendorDependentOperands>;

} // namespace ASFW::AVC::Cmd
