// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// GeneralCommands.hpp - AV/C General commands: UNIT INFO, SUBUNIT INFO,
// PLUG INFO, INPUT/OUTPUT PLUG SIGNAL FORMAT, VENDOR-DEPENDENT.
//
// Layouts: Linux ta1394 general/src/general.rs (MIT), cited per command; TA 2004006
// AV/C General 4.2 where noted. Where ASFW already sends a command that works on
// hardware, the new codec must produce the SAME bytes (differential test,
// docs/avc-rebuild/phase-1.md). Fresh implementation; no reference code copied.
//
// Pattern for every command: Build*() returns the full CommandFrame; Parse*()
// takes a Response from ParseResponseFor() and checks the response code itself
// (STATUS -> IMPLEMENTED/STABLE, CONTROL -> ACCEPTED).
//
// Implementation: GeneralCommands.cpp (phase 1).

#pragma once

#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"
#include "../Core/RateCodes.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::AVC::Cmd {

// ---------------------------------------------------------------------------
// UNIT INFO (0x30), STATUS, unit address only.
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
};

[[nodiscard]] Expected<CommandFrame> BuildUnitInfoStatus(
    UnitInfoStyle style = UnitInfoStyle::kStandardAppleLegacy) noexcept;
[[nodiscard]] Expected<UnitInfo> ParseUnitInfo(const Response& response) noexcept;

// ---------------------------------------------------------------------------
// SUBUNIT INFO (0x31), STATUS, unit address only.
// Command operands: [page<<4 | extension code] FF FF FF FF.
// Response operands: [0] echoes page/extension code; [1..4] up to four entries,
// each type<<3 | max id, 0xFF = empty. ta1394 general.rs `SubunitInfo` (OPCODE at :130).
// Today's ASFW sends extension code 7 (AVCCommands.hpp:181), as does ta1394.
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

[[nodiscard]] Expected<CommandFrame> BuildSubunitInfoStatus(uint8_t page, uint8_t extensionCode = 0x07) noexcept;
[[nodiscard]] Expected<SubunitInfo> ParseSubunitInfo(const Response& response) noexcept;

// ---------------------------------------------------------------------------
// PLUG INFO (0x02), STATUS. ta1394 general.rs `PlugInfo` (OPCODE at :378).
// Command operands: [subfunction] FF FF FF FF.
//   Unit, subfunction 0x00: [1]=iso in, [2]=iso out, [3]=external in, [4]=external out.
//   Unit, subfunction 0x01: [1]=async in, [2]=async out.
//   Subunit, subfunction 0x00: [1]=destination plugs, [2]=source plugs.
// The response must echo the subfunction (kMalformedOperands at offset 0 otherwise).
// BridgeCo's extended form (subfunction 0xC0) is in Extensions/BridgeCoPlugInfo.hpp.
// ---------------------------------------------------------------------------

enum class UnitPlugInfoKind : uint8_t {
    kIsochronousExternal = 0x00,
    kAsynchronous = 0x01,
};

struct UnitIsochronousExternalPlugs {
    uint8_t isochronousInputs{0};
    uint8_t isochronousOutputs{0};
    uint8_t externalInputs{0};
    uint8_t externalOutputs{0};
};

struct UnitAsynchronousPlugs {
    uint8_t asynchronousInputs{0};
    uint8_t asynchronousOutputs{0};
};

struct SubunitPlugs {
    uint8_t destinationPlugs{0};
    uint8_t sourcePlugs{0};
};

[[nodiscard]] Expected<CommandFrame> BuildUnitPlugInfoStatus(UnitPlugInfoKind kind) noexcept;
/// `subunit` must not be the unit address (kInvalidArgument).
[[nodiscard]] Expected<CommandFrame> BuildSubunitPlugInfoStatus(SubunitAddress subunit) noexcept;

[[nodiscard]] Expected<UnitIsochronousExternalPlugs> ParseUnitIsochronousExternalPlugs(
    const Response& response) noexcept;
[[nodiscard]] Expected<UnitAsynchronousPlugs> ParseUnitAsynchronousPlugs(const Response& response) noexcept;
[[nodiscard]] Expected<SubunitPlugs> ParseSubunitPlugs(const Response& response) noexcept;

// ---------------------------------------------------------------------------
// INPUT (0x19) / OUTPUT (0x18) PLUG SIGNAL FORMAT. Unit address only.
// General 4.2 §10.10 / §10.11; ta1394 general.rs `PlugSignalFormat` (OPCODEs at :521, :560);
// Linux fcp.c `avc_general_set_sig_fmt` / `avc_general_get_sig_fmt`.
// Operands: [plug id][FMT][FDF 3 bytes].
//   CONTROL AM824: FMT 0x90, FDF = [sfc & 0x07] FF FF   (Linux fcp.c:58-61)
//   STATUS query:  see SignalFormatQuery.
// CONTROL expects ACCEPTED, STATUS expects IMPLEMENTED/STABLE; both echo the layout.
// ---------------------------------------------------------------------------

enum class PlugSignalDirection : uint8_t {
    kInput,   ///< opcode 0x19: a plug the device RECEIVES on (host playback).
    kOutput,  ///< opcode 0x18: a plug the device SENDS on (host capture).
};

/// What a STATUS query puts after the plug id. Two references disagree; the
/// phase-1 hardware matrix records how each device answers both.
enum class SignalFormatQuery : uint8_t {
    kAllWildcard,    ///< FF FF FF FF: ta1394 general.rs, and what ASFW sends today
                     ///< (AVCUnitPlugSignalFormatCommand.hpp:135-139, hardware-proven). Default.
    kAm824Wildcard,  ///< 90 FF FF FF: Linux fcp.c:103-106 (FDF-hi 0xFF at :104).
};

inline constexpr uint8_t kFmtAm824 = 0x90;  ///< Linux fcp.c:58 "EOH_1, Form_1, FMT. AM824".

struct PlugSignalFormat {
    uint8_t plugId{0};
    uint8_t fmt{0xFF};
    std::array<uint8_t, 3> fdf{0xFF, 0xFF, 0xFF};
};

/// The AM824 format for `sfc`: FMT 0x90, FDF [sfc] FF FF.
[[nodiscard]] constexpr PlugSignalFormat Am824SignalFormat(uint8_t plugId, CipSfc sfc) noexcept {
    return PlugSignalFormat{plugId, kFmtAm824,
                            {static_cast<uint8_t>(static_cast<uint8_t>(sfc) & 0x07), 0xFF, 0xFF}};
}

/// The SFC of an AM824 format, or nullopt when FMT is not AM824.
[[nodiscard]] constexpr std::optional<CipSfc> SfcOf(const PlugSignalFormat& format) noexcept {
    if (format.fmt != kFmtAm824) {
        return std::nullopt;
    }
    return static_cast<CipSfc>(format.fdf[0] & 0x07);
}

[[nodiscard]] Expected<CommandFrame> BuildPlugSignalFormatStatus(
    PlugSignalDirection direction, uint8_t plugId,
    SignalFormatQuery query = SignalFormatQuery::kAllWildcard) noexcept;

[[nodiscard]] Expected<CommandFrame> BuildPlugSignalFormatControl(PlugSignalDirection direction,
                                                                  const PlugSignalFormat& format) noexcept;

/// `expected`: kImplementedStable for STATUS, kAccepted for CONTROL.
[[nodiscard]] Expected<PlugSignalFormat> ParsePlugSignalFormat(const Response& response,
                                                               ResponseCode expected) noexcept;

// ---------------------------------------------------------------------------
// VENDOR-DEPENDENT (0x00). Any address, any command type.
// Operands: [company ID 3 bytes][vendor payload]. ta1394 general.rs `VendorDependent` (OPCODE at :226)
// (payload must be non-empty; response must carry more than the company ID).
// ---------------------------------------------------------------------------

/// A VIEW into the response buffer; valid while that buffer lives.
struct VendorDependentReply {
    CompanyId companyId{0xFF, 0xFF, 0xFF};
    std::span<const uint8_t> payload{};
};

[[nodiscard]] Expected<CommandFrame> BuildVendorDependent(CommandType type, SubunitAddress address,
                                                          const CompanyId& companyId,
                                                          std::span<const uint8_t> payload) noexcept;

[[nodiscard]] Expected<VendorDependentReply> ParseVendorDependent(const Response& response,
                                                                  ResponseCode expected) noexcept;

} // namespace ASFW::AVC::Cmd
