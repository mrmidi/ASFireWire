// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// CcmTypes.hpp - The AV/C Connection and Compatibility Management vocabulary:
// plug numbers, signal addresses, status fields and their named values.
//
// Source: TA Document 2002010, AV/C Connection and Compatibility Management
// Specification 1.1 (19 Mar 2003), referred to below as "CCM". Table, Figure and
// section numbers are that document's. Cross-checked against Apple AppleFWAudio
// (AM824AVC::GetSignalSourceInfo, SyncPlugReconnect) and FFADO
// libavc/ccm/avc_signal_source.{h,cpp}. Fresh implementation; no reference code copied.
//
// Contract:
// - Callers name what they mean (UnitPlugId::SerialBus(0), SignalAddress::NoSignal(),
//   OutputStatus::kReady). A raw byte only exists inside these types and in the codecs
//   that serialise them.
// - Reading is lenient: every field keeps its raw value, and a named view returns
//   std::nullopt for a reserved value. A device that sends a reserved value is not a
//   parse error. CheckStatusAgainstSpec() reports the departure separately.

#pragma once

#include "../Core/AvcTypes.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace ASFW::AVC::Cmd {

// ---------------------------------------------------------------------------
// Unit plug numbers (CCM Tables 7.2, 7.4, 7.11-7.13, 7.15, 7.16, 7.19, 7.20, 7.22, 7.24)
// ---------------------------------------------------------------------------

/// A unit plug number as a plug_ID field carries it: serial bus PCR 0..30, external
/// plug 0..30, or one of a few named special values. The same value can mean
/// different things in different fields, so each meaning has its own factory.
class UnitPlugId {
public:
    static constexpr uint8_t kMaxPlugNumber = 0x1E;     ///< PCR[0..30], external plug zero..30.
    static constexpr uint8_t kExternalBase = 0x80;      ///< External plug zero.

    /// Serial bus iPCR / oPCR number.
    [[nodiscard]] static constexpr UnitPlugId SerialBus(uint8_t number) noexcept {
        return UnitPlugId{static_cast<uint8_t>(number & 0x7F)};
    }
    /// External input / output plug number.
    [[nodiscard]] static constexpr UnitPlugId External(uint8_t number) noexcept {
        return UnitPlugId{static_cast<uint8_t>(kExternalBase | (number & 0x7F))};
    }
    /// "Any available serial bus plug" (Tables 7.2, 7.4, 7.16, 7.24).
    [[nodiscard]] static constexpr UnitPlugId AnyAvailableSerialBus() noexcept { return UnitPlugId{0x7F}; }
    /// "Any available external plug" (Tables 7.2, 7.4, 7.15, 7.16, 7.24).
    [[nodiscard]] static constexpr UnitPlugId AnyAvailableExternal() noexcept { return UnitPlugId{0xFF}; }
    /// "Invalid" (Tables 7.2-7.5, 7.15, 7.19, 7.20).
    [[nodiscard]] static constexpr UnitPlugId Invalid() noexcept { return UnitPlugId{0xFE}; }
    /// A value read from the wire, kept as is.
    [[nodiscard]] static constexpr UnitPlugId FromRaw(uint8_t raw) noexcept { return UnitPlugId{raw}; }

    [[nodiscard]] constexpr uint8_t Raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr bool IsSerialBus() const noexcept { return raw_ <= kMaxPlugNumber; }
    [[nodiscard]] constexpr bool IsExternal() const noexcept {
        return raw_ >= kExternalBase && raw_ <= (kExternalBase | kMaxPlugNumber);
    }
    /// The plug number of a serial bus or external plug; nullopt for a special or reserved value.
    [[nodiscard]] constexpr std::optional<uint8_t> Number() const noexcept {
        if (IsSerialBus()) return raw_;
        if (IsExternal()) return static_cast<uint8_t>(raw_ & 0x7F);
        return std::nullopt;
    }

    friend constexpr bool operator==(UnitPlugId, UnitPlugId) noexcept = default;

private:
    explicit constexpr UnitPlugId(uint8_t raw) noexcept : raw_(raw) {}
    uint8_t raw_;
};

static_assert(UnitPlugId::SerialBus(0).IsSerialBus() && !UnitPlugId::SerialBus(0).IsExternal());
static_assert(UnitPlugId::External(0).IsExternal() && UnitPlugId::External(0).Number() == 0);
static_assert(UnitPlugId::External(30).Number() == 30);
static_assert(!UnitPlugId::AnyAvailableSerialBus().Number().has_value());
static_assert(!UnitPlugId::Invalid().Number().has_value());

// ---------------------------------------------------------------------------
// Signal address: signal_source / signal_destination (CCM Figures 7.2-7.5, 7.11-7.16)
// ---------------------------------------------------------------------------

/// What a signal address field refers to.
enum class SignalAddressKind : uint8_t {
    kUnitPlug,     ///< A unit input / output plug (first byte FF, plug_ID decides which).
    kSubunitPlug,  ///< A subunit source / destination plug (first byte is the subunit address).
    kNoSignal,     ///< FF FE: "no signal source" (Fig 7.14), "no signal destination" (Fig 7.18, 7.29).
    kMultiple,     ///< FF FD: "multiple sources" (Fig 7.16), "multiple" destinations (Fig 7.22, 7.26).
    kAvConvergent, ///< FF FC: independent audio and video sources joined by CONNECT AV (Fig 7.15).
};

/// A two-byte signal address: [subunit address, or FF for the unit][plug_ID]. Build one
/// with a named factory; `bytes` is the wire encoding, read by serialisers.
struct SignalAddress {
    static constexpr uint8_t kUnitAddressByte = SubunitAddress::Unit().Byte();
    static constexpr uint8_t kNoSignalPlugId = 0xFE;
    static constexpr uint8_t kMultiplePlugId = 0xFD;
    static constexpr uint8_t kAvConvergentPlugId = 0xFC;
    static constexpr uint8_t kAnyAvailableSubunitPlugId = 0xFF;  ///< Tables 7.3, 7.5, 7.17, 7.25.

    std::array<uint8_t, 2> bytes{kUnitAddressByte, kNoSignalPlugId};

    /// A unit plug (input or output, by position in the command).
    [[nodiscard]] static constexpr SignalAddress OfUnitPlug(UnitPlugId plug) noexcept {
        return SignalAddress{{kUnitAddressByte, plug.Raw()}};
    }
    [[nodiscard]] static constexpr SignalAddress UnitIsochronousPlug(uint8_t plugId) noexcept {
        return OfUnitPlug(UnitPlugId::SerialBus(plugId));
    }
    [[nodiscard]] static constexpr SignalAddress UnitExternalPlug(uint8_t plugId) noexcept {
        return OfUnitPlug(UnitPlugId::External(plugId));
    }
    [[nodiscard]] static constexpr SignalAddress AnyAvailableSerialBusPlug() noexcept {
        return OfUnitPlug(UnitPlugId::AnyAvailableSerialBus());
    }
    [[nodiscard]] static constexpr SignalAddress AnyAvailableExternalPlug() noexcept {
        return OfUnitPlug(UnitPlugId::AnyAvailableExternal());
    }
    [[nodiscard]] static constexpr SignalAddress SubunitPlug(SubunitAddress subunit, uint8_t plugId) noexcept {
        return SignalAddress{{subunit.Byte(), plugId}};
    }
    [[nodiscard]] static constexpr SignalAddress AnyAvailableSubunitPlug(SubunitAddress subunit) noexcept {
        return SubunitPlug(subunit, kAnyAvailableSubunitPlugId);
    }
    /// "No signal source" / "no signal destination" / the STATUS command's source placeholder.
    [[nodiscard]] static constexpr SignalAddress NoSignal() noexcept {
        return SignalAddress{{kUnitAddressByte, kNoSignalPlugId}};
    }
    [[nodiscard]] static constexpr SignalAddress Multiple() noexcept {
        return SignalAddress{{kUnitAddressByte, kMultiplePlugId}};
    }
    [[nodiscard]] static constexpr SignalAddress AvConvergent() noexcept {
        return SignalAddress{{kUnitAddressByte, kAvConvergentPlugId}};
    }

    [[nodiscard]] constexpr SignalAddressKind Kind() const noexcept {
        if (bytes[0] != kUnitAddressByte) return SignalAddressKind::kSubunitPlug;
        switch (bytes[1]) {
            case kNoSignalPlugId: return SignalAddressKind::kNoSignal;
            case kMultiplePlugId: return SignalAddressKind::kMultiple;
            case kAvConvergentPlugId: return SignalAddressKind::kAvConvergent;
            default: return SignalAddressKind::kUnitPlug;
        }
    }

    /// The first byte is FF (a unit plug or one of the special unit values).
    [[nodiscard]] constexpr bool IsUnit() const noexcept { return bytes[0] == kUnitAddressByte; }
    [[nodiscard]] constexpr bool IsExternalUnitPlug() const noexcept {
        return IsUnit() && UnitPlug().IsExternal();
    }
    /// Plug number: the unit plug number for a unit address (external flag removed), the plug_ID otherwise.
    [[nodiscard]] constexpr uint8_t PlugId() const noexcept {
        return IsUnit() ? static_cast<uint8_t>(bytes[1] & 0x7F) : bytes[1];
    }
    [[nodiscard]] constexpr UnitPlugId UnitPlug() const noexcept { return UnitPlugId::FromRaw(bytes[1]); }
    [[nodiscard]] constexpr SubunitAddress Subunit() const noexcept { return SubunitAddress::FromByte(bytes[0]); }

    friend constexpr bool operator==(const SignalAddress&, const SignalAddress&) noexcept = default;
};

static_assert(SignalAddress::NoSignal().Kind() == SignalAddressKind::kNoSignal);
static_assert(SignalAddress::Multiple().Kind() == SignalAddressKind::kMultiple);
static_assert(SignalAddress::AvConvergent().Kind() == SignalAddressKind::kAvConvergent);
static_assert(SignalAddress::UnitIsochronousPlug(2).Kind() == SignalAddressKind::kUnitPlug);
static_assert(SignalAddress::SubunitPlug(kMusicSubunit0, 0).Kind() == SignalAddressKind::kSubunitPlug);
static_assert(SignalAddress{}.Kind() == SignalAddressKind::kNoSignal);

/// The kind of plug a SIGNAL SOURCE status response describes. It decides which
/// output_status values and which conv value are legal (CCM Tables 7.7-7.10).
enum class DestinationPlugKind : uint8_t {
    kSerialBusOutputPlug,    ///< oPCR (Table 7.7).
    kExternalOutputPlug,     ///< External output plug (Table 7.8).
    kSubunitDestinationPlug, ///< Subunit destination plug (Table 7.9).
};

/// The plug kind of a destination address; nullopt for the special values (no signal, multiple, ...)
/// and for a unit plug number outside the defined ranges.
[[nodiscard]] constexpr std::optional<DestinationPlugKind> DestinationKindOf(const SignalAddress& destination) noexcept {
    switch (destination.Kind()) {
        case SignalAddressKind::kSubunitPlug:
            return DestinationPlugKind::kSubunitDestinationPlug;
        case SignalAddressKind::kUnitPlug:
            if (destination.UnitPlug().IsSerialBus()) return DestinationPlugKind::kSerialBusOutputPlug;
            if (destination.UnitPlug().IsExternal()) return DestinationPlugKind::kExternalOutputPlug;
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// SIGNAL SOURCE STATUS response, operand[0] (CCM §7.1.5, Figure 7.8)
// ---------------------------------------------------------------------------

/// output_status: the state of the destination plug (Table 7.7; Annex A.2 for the
/// state model). Tables 7.8 and 7.9 allow only kEffective and kNotEffective.
enum class OutputStatus : uint8_t {
    kEffective = 0x0,            ///< Data packets are flowing through the oPCR / the signal reaches the plug.
    kNotEffective = 0x1,         ///< No signal flows to the plug (no internal connection, or nothing from the source).
    kInsufficientResource = 0x2, ///< The oPCR sends only empty packets: bus bandwidth is short.
    kReady = 0x3,                ///< The signal reaches the oPCR but no isochronous connection exists (PCR-ready).
    kVirtualOutput = 0x4,        ///< The oPCR repeats the stream the unit receives on an iPCR.
};

/// signal_status: four flags saying how the signal at the destination differs from the
/// source (Figure 7.10). All clear means identical.
struct SignalModifications {
    static constexpr uint8_t kOsdOverlaidBit = 0x1;
    static constexpr uint8_t kConvertedBit = 0x2;
    static constexpr uint8_t kFilteredBit = 0x4;
    static constexpr uint8_t kProcessedBit = 0x8;

    uint8_t bits{0};

    [[nodiscard]] constexpr bool Identical() const noexcept { return bits == 0; }
    [[nodiscard]] constexpr bool Processed() const noexcept { return (bits & kProcessedBit) != 0; }
    [[nodiscard]] constexpr bool Filtered() const noexcept { return (bits & kFilteredBit) != 0; }
    [[nodiscard]] constexpr bool Converted() const noexcept { return (bits & kConvertedBit) != 0; }
    [[nodiscard]] constexpr bool OsdOverlaid() const noexcept { return (bits & kOsdOverlaidBit) != 0; }

    friend constexpr bool operator==(SignalModifications, SignalModifications) noexcept = default;
};

/// operand[0] of a SIGNAL SOURCE STATUS command and response:
/// output_status (bits 7..5) | conv (bit 4) | signal_status (bits 3..0).
class SignalSourceStatusField {
public:
    static constexpr unsigned kOutputStatusShift = 5;
    static constexpr uint8_t kOutputStatusMask = 0x07;
    static constexpr uint8_t kConvBit = 0x10;
    static constexpr uint8_t kSignalStatusMask = 0x0F;

    /// The values a controller puts in a STATUS command: output_status 7, conv 1,
    /// signal_status F (Table C.1, "default" column; Figure 7.7 shows the byte as FF).
    static constexpr uint8_t kRequestOutputStatus = 0x7;
    static constexpr bool kRequestConv = true;
    static constexpr uint8_t kRequestSignalStatus = 0xF;

    [[nodiscard]] static constexpr SignalSourceStatusField Request() noexcept {
        return Make(kRequestOutputStatus, kRequestConv, kRequestSignalStatus);
    }
    [[nodiscard]] static constexpr SignalSourceStatusField Make(uint8_t outputStatusCode, bool conv,
                                                                uint8_t signalStatus) noexcept {
        return SignalSourceStatusField{static_cast<uint8_t>(
            ((outputStatusCode & kOutputStatusMask) << kOutputStatusShift) | (conv ? kConvBit : 0) |
            (signalStatus & kSignalStatusMask))};
    }
    [[nodiscard]] static constexpr SignalSourceStatusField FromRaw(uint8_t raw) noexcept {
        return SignalSourceStatusField{raw};
    }

    [[nodiscard]] constexpr uint8_t Raw() const noexcept { return raw_; }
    /// The three output_status bits as sent, reserved values included.
    [[nodiscard]] constexpr uint8_t OutputStatusCode() const noexcept {
        return static_cast<uint8_t>((raw_ >> kOutputStatusShift) & kOutputStatusMask);
    }
    /// output_status by name; nullopt for a reserved value (5..7).
    [[nodiscard]] constexpr std::optional<OutputStatus> Status() const noexcept {
        const uint8_t code = OutputStatusCode();
        if (code > static_cast<uint8_t>(OutputStatus::kVirtualOutput)) return std::nullopt;
        return static_cast<OutputStatus>(code);
    }
    /// conv: the transfer format at the destination oPCR can be changed with OUTPUT PLUG
    /// SIGNAL FORMAT (Table 7.10). Defined for serial bus oPCRs only.
    [[nodiscard]] constexpr bool CanChangeFormat() const noexcept { return (raw_ & kConvBit) != 0; }
    [[nodiscard]] constexpr SignalModifications Modifications() const noexcept {
        return SignalModifications{static_cast<uint8_t>(raw_ & kSignalStatusMask)};
    }

    friend constexpr bool operator==(SignalSourceStatusField, SignalSourceStatusField) noexcept = default;

private:
    explicit constexpr SignalSourceStatusField(uint8_t raw) noexcept : raw_(raw) {}
    uint8_t raw_;
};

static_assert(SignalSourceStatusField::Request().Raw() == 0xFF);  // CCM Table C.1: command frame operand[0]
// Table C.1 reply: output_status 3 (ready), conv 0, signal_status 0.
static_assert(SignalSourceStatusField::Make(3, false, 0).Raw() == 0x60);
static_assert(SignalSourceStatusField::FromRaw(0x70).Status() == OutputStatus::kReady);
static_assert(SignalSourceStatusField::FromRaw(0x70).CanChangeFormat());
static_assert(!SignalSourceStatusField::FromRaw(0xA0).Status().has_value());  // output_status 5 is reserved

// ---------------------------------------------------------------------------
// SIGNAL SOURCE CONTROL command / response, operand[0] (CCM §7.1.1, §7.1.2, Figures 7.1, 7.6)
// ---------------------------------------------------------------------------

/// result_status of a SIGNAL SOURCE control response (Table 7.6).
enum class SignalSourceResult : uint8_t {
    kSource = 0x0,         ///< The path is set up and the signal source can generate the signal. ACCEPTED.
    kNotSource = 0x1,      ///< The path is set up but the plug only passes a signal through. ACCEPTED.
    kNoInformation = 0xF,  ///< No information. REJECTED.
};

/// operand[0] of a SIGNAL SOURCE CONTROL / SPECIFIC INQUIRY command and of a control
/// response: reserved (bits 7..4, zero) | result_status (bits 3..0).
class SignalSourceControlField {
public:
    static constexpr uint8_t kResultMask = 0x0F;
    static constexpr uint8_t kReservedMask = 0xF0;

    /// What a controller sends: reserved 0, result_status F (Figure 7.1 shows F; Table C.2
    /// "default"). The spec reserves the upper nibble as zero so a REJECTED reply to a
    /// control command can be told from one to a STATUS command (§7.1.1).
    [[nodiscard]] static constexpr SignalSourceControlField Request() noexcept {
        return SignalSourceControlField{static_cast<uint8_t>(SignalSourceResult::kNoInformation)};
    }
    [[nodiscard]] static constexpr SignalSourceControlField FromRaw(uint8_t raw) noexcept {
        return SignalSourceControlField{raw};
    }

    [[nodiscard]] constexpr uint8_t Raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr uint8_t ResultCode() const noexcept { return static_cast<uint8_t>(raw_ & kResultMask); }
    /// result_status by name; nullopt for a reserved value (2..E).
    [[nodiscard]] constexpr std::optional<SignalSourceResult> Result() const noexcept {
        switch (ResultCode()) {
            case static_cast<uint8_t>(SignalSourceResult::kSource): return SignalSourceResult::kSource;
            case static_cast<uint8_t>(SignalSourceResult::kNotSource): return SignalSourceResult::kNotSource;
            case static_cast<uint8_t>(SignalSourceResult::kNoInformation): return SignalSourceResult::kNoInformation;
            default: return std::nullopt;
        }
    }
    /// The reserved upper nibble was zero, as the spec requires.
    [[nodiscard]] constexpr bool ReservedIsZero() const noexcept { return (raw_ & kReservedMask) == 0; }

    friend constexpr bool operator==(SignalSourceControlField, SignalSourceControlField) noexcept = default;

private:
    explicit constexpr SignalSourceControlField(uint8_t raw) noexcept : raw_(raw) {}
    uint8_t raw_;
};

static_assert(SignalSourceControlField::Request().Raw() == 0x0F);  // CCM Figure 7.1, Table C.2
static_assert(SignalSourceControlField::FromRaw(0x00).Result() == SignalSourceResult::kSource);
static_assert(!SignalSourceControlField::FromRaw(0x05).Result().has_value());

/// The first operand of a SIGNAL SOURCE response, kept as sent. Its meaning depends on
/// the command type that was answered: a STATUS reply carries output_status, conv and
/// signal_status; a CONTROL (and SPECIFIC INQUIRY) reply carries result_status.
class SignalSourceFirstOperand {
public:
    [[nodiscard]] static constexpr SignalSourceFirstOperand FromRaw(uint8_t raw) noexcept {
        return SignalSourceFirstOperand{raw};
    }
    [[nodiscard]] constexpr uint8_t Raw() const noexcept { return raw_; }
    [[nodiscard]] constexpr SignalSourceStatusField AsStatus() const noexcept {
        return SignalSourceStatusField::FromRaw(raw_);
    }
    [[nodiscard]] constexpr SignalSourceControlField AsControl() const noexcept {
        return SignalSourceControlField::FromRaw(raw_);
    }

    friend constexpr bool operator==(SignalSourceFirstOperand, SignalSourceFirstOperand) noexcept = default;

private:
    explicit constexpr SignalSourceFirstOperand(uint8_t raw) noexcept : raw_(raw) {}
    uint8_t raw_;
};

// ---------------------------------------------------------------------------
// Lenient parse, strict interpretation
// ---------------------------------------------------------------------------

/// Where a STATUS response departs from the spec. Every flag is information about the
/// device, not a parse failure: the response stays usable.
struct StatusDeviations {
    /// output_status 5..7, reserved for every plug kind (Tables 7.7-7.9).
    bool reservedOutputStatus{false};
    /// output_status is a defined value, but not one this plug kind allows: an external
    /// output plug or a subunit destination plug may only report effective or not
    /// effective (Tables 7.8, 7.9).
    bool outputStatusNotAllowedForPlug{false};
    /// conv is set on a plug that is not a serial bus oPCR; it must be zero there (Table 7.10).
    bool convSetOnNonSerialBusPlug{false};

    [[nodiscard]] constexpr bool Any() const noexcept {
        return reservedOutputStatus || outputStatusNotAllowedForPlug || convSetOnNonSerialBusPlug;
    }
    friend constexpr bool operator==(StatusDeviations, StatusDeviations) noexcept = default;
};

/// Check a STATUS response's first operand against the spec for the kind of plug it describes.
[[nodiscard]] constexpr StatusDeviations CheckStatusAgainstSpec(SignalSourceStatusField status,
                                                                DestinationPlugKind plug) noexcept {
    StatusDeviations deviations;
    const auto named = status.Status();
    if (!named) {
        deviations.reservedOutputStatus = true;
    } else if (plug != DestinationPlugKind::kSerialBusOutputPlug && *named != OutputStatus::kEffective &&
               *named != OutputStatus::kNotEffective) {
        deviations.outputStatusNotAllowedForPlug = true;
    }
    if (plug != DestinationPlugKind::kSerialBusOutputPlug && status.CanChangeFormat()) {
        deviations.convSetOnNonSerialBusPlug = true;
    }
    return deviations;
}

// The Phase 88 reports 0x70 on a subunit destination plug: ready, conv set.
static_assert(CheckStatusAgainstSpec(SignalSourceStatusField::FromRaw(0x70),
                                     DestinationPlugKind::kSubunitDestinationPlug)
                  .outputStatusNotAllowedForPlug);
static_assert(CheckStatusAgainstSpec(SignalSourceStatusField::FromRaw(0x70),
                                     DestinationPlugKind::kSubunitDestinationPlug)
                  .convSetOnNonSerialBusPlug);
static_assert(!CheckStatusAgainstSpec(SignalSourceStatusField::FromRaw(0x70),
                                      DestinationPlugKind::kSerialBusOutputPlug)
                   .Any());
static_assert(!CheckStatusAgainstSpec(SignalSourceStatusField::Make(3, false, 0),
                                      DestinationPlugKind::kSerialBusOutputPlug)
                   .Any());

} // namespace ASFW::AVC::Cmd
