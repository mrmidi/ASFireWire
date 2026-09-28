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
// By default, sends 0 operands [01, FF, 30] (quadlet-padded to 4 bytes), matching
// Apple's AppleFWAudio, legacy ASFW, and FireBug hardware traces.
// The Linux ta1394 5-dummy-operand form [07, FF, FF, FF, FF] is available via UnitInfoStyle.
// Response operands: [0]=07, [1]=unit type<<3|id, [2..4]=company ID.
// ---------------------------------------------------------------------------

enum class UnitInfoStyle : uint8_t {
    kStandardAppleLegacy = 0,   ///< 0 operands: [01, FF, 30] (Apple + legacy ASFW)
    kLinuxFiveDummyOperands = 1, ///< 5 dummy operands: [07, FF, FF, FF, FF] (Linux ta1394)
};

struct UnitInfo {
    SubunitType unitType{SubunitType::kUnit};
    uint8_t unitId{0x07};
    CompanyId companyId{0xFF, 0xFF, 0xFF};

    friend constexpr bool operator==(const UnitInfo&, const UnitInfo&) noexcept = default;
};

struct UnitInfoOperands {
    static constexpr Opcode kOpcode = Opcode::kUnitInfo;
    static constexpr bool kRequiresUnitAddress = true;

    UnitInfoStyle style{UnitInfoStyle::kStandardAppleLegacy};

    using Reply = UnitInfo;

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t != CommandType::kStatus) {
            return Fail(AvcErrorKind::kInvalidArgument);
        }
        if (style == UnitInfoStyle::kLinuxFiveDummyOperands) {
            constexpr std::array<uint8_t, 5> kOperands = {0x07, 0xFF, 0xFF, 0xFF, 0xFF};
            return w.Append(kOperands);
        }
        return {}; // 0 operands
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < 5) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        if (in[0] != 0x07) {
            return FailAt(AvcErrorKind::kMalformedOperands, 0);
        }

        return UnitInfo{
            .unitType = static_cast<SubunitType>((in[1] >> 3) & 0x1F),
            .unitId = static_cast<uint8_t>(in[1] & 0x07),
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

struct SubunitInfoEntry {
    SubunitType type{SubunitType::kUnit};
    uint8_t maximumId{0};
};

struct SubunitInfo {
    uint8_t page{0};
    uint8_t extensionCode{0x07};
    std::array<SubunitInfoEntry, 4> entries{};
    uint8_t entryCount{0};  ///< Entries before the first 0xFF slot.
};

struct SubunitInfoOperands {
    static constexpr Opcode kOpcode = Opcode::kSubunitInfo;
    static constexpr bool kRequiresUnitAddress = true;

    uint8_t page{0};
    uint8_t extensionCode{0x07};

    using Reply = SubunitInfo;

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        if (t != CommandType::kStatus) {
            return Fail(AvcErrorKind::kInvalidArgument);
        }
        const std::array<uint8_t, 5> ops = {
            static_cast<uint8_t>(((page & 0x07) << 4) | (extensionCode & 0x07)),
            0xFF, 0xFF, 0xFF, 0xFF
        };
        return w.Append(ops);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < 5) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }

        SubunitInfo info{
            .page = static_cast<uint8_t>((in[0] >> 4) & 0x07),
            .extensionCode = static_cast<uint8_t>(in[0] & 0x07),
            .entries = {},
            .entryCount = 0,
        };

        for (size_t i = 0; i < 4; ++i) {
            const uint8_t entryByte = in[1 + i];
            if (entryByte == 0xFF) {
                break;
            }
            info.entries[info.entryCount++] = SubunitInfoEntry{
                .type = static_cast<SubunitType>((entryByte >> 3) & 0x1F),
                .maximumId = static_cast<uint8_t>(entryByte & 0x07),
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
    uint8_t dummyByte{0xFF};
    using Reply = PlugInfoReply;

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
        const uint8_t subfunction = form == PlugInfoForm::kUnitAsync ? 0x01 : 0x00;
        const std::array<uint8_t, 5> ops = {subfunction, dummyByte, dummyByte, dummyByte, dummyByte};
        return w.Append(ops);
    }

    [[nodiscard]] Expected<Reply> Read(std::span<const uint8_t> in) const noexcept {
        const uint8_t subfunction = form == PlugInfoForm::kUnitAsync ? 0x01 : 0x00;
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

inline constexpr uint8_t kFmtAm824 = 0x90;

struct PlugSignalFormat {
    uint8_t plugId{0};
    uint8_t fmt{0xFF};
    std::array<uint8_t, 3> fdf{0xFF, 0xFF, 0xFF};
};

[[nodiscard]] constexpr PlugSignalFormat Am824SignalFormat(uint8_t plugId, CipSfc sfc) noexcept {
    return PlugSignalFormat{plugId, kFmtAm824,
                            {static_cast<uint8_t>(static_cast<uint8_t>(sfc) & 0x07), 0xFF, 0xFF}};
}

[[nodiscard]] constexpr std::optional<CipSfc> SfcOf(const PlugSignalFormat& format) noexcept {
    if (format.fmt != kFmtAm824) {
        return std::nullopt;
    }
    return static_cast<CipSfc>(format.fdf[0] & 0x07);
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
                (query == SignalFormatQuery::kAm824Wildcard) ? kFmtAm824 : uint8_t{0xFF},
                0xFF, 0xFF, 0xFF
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

/// Owned VENDOR-DEPENDENT operands; the company ID and payload are supplied by the caller.
struct RawVendorDependentOperands {
    static constexpr Opcode kOpcode = Opcode::kVendorDependent;

    CompanyId companyId{0xFF, 0xFF, 0xFF};
    std::array<uint8_t, 256> payloadBytes{};
    uint16_t payloadLength{0};

    using Reply = std::vector<uint8_t>;

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
        return std::vector<uint8_t>(in.begin() + 3, in.end());
    }
};

using RawVendorDependentCommand = Command<RawVendorDependentOperands>;

} // namespace ASFW::AVC::Cmd
