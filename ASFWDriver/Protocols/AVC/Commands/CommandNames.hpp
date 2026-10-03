// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// CommandNames.hpp - Spec names for command fields and replies, for logs and diagnostics.
//
// Same contract as Core/AvcNames.hpp: `name(0xNN)` for a value the table knows,
// `UNKNOWN(<table>:0xNN)` for one it does not. Each table cites the spec table it names.

#pragma once

#include "CcmTypes.hpp"
#include "DescriptorCommands.hpp"
#include "FunctionBlockCommand.hpp"
#include "GeneralCommands.hpp"
#include "SignalSourceCommand.hpp"
#include "StreamFormatCommand.hpp"
#include "../Core/AvcNames.hpp"
#include "../Core/RateCodes.hpp"

#include <string>

namespace ASFW::AVC {

namespace names {

// TA 2001002 Table 5.6; 0x0A is BridgeCo's 88.2 kHz (not in that table; Linux bebob_stream.c:38-46).
inline constexpr std::array kStreamFormatRates{
    NameEntry{static_cast<uint32_t>(StreamFormatRate::k22050), "22.05 kHz"},
    NameEntry{static_cast<uint32_t>(StreamFormatRate::k24000), "24 kHz"},
    NameEntry{static_cast<uint32_t>(StreamFormatRate::k32000), "32 kHz"},
    NameEntry{static_cast<uint32_t>(StreamFormatRate::k44100), "44.1 kHz"},
    NameEntry{static_cast<uint32_t>(StreamFormatRate::k48000), "48 kHz"},
    NameEntry{static_cast<uint32_t>(StreamFormatRate::k96000), "96 kHz"},
    NameEntry{static_cast<uint32_t>(StreamFormatRate::k176400), "176.4 kHz"},
    NameEntry{static_cast<uint32_t>(StreamFormatRate::k192000), "192 kHz"},
    NameEntry{static_cast<uint32_t>(StreamFormatRate::k88200), "88.2 kHz"},
};

// IEC 61883-6 Table 20.
inline constexpr std::array kCipSfcs{
    NameEntry{static_cast<uint32_t>(CipSfc::k32000), "32 kHz"},
    NameEntry{static_cast<uint32_t>(CipSfc::k44100), "44.1 kHz"},
    NameEntry{static_cast<uint32_t>(CipSfc::k48000), "48 kHz"},
    NameEntry{static_cast<uint32_t>(CipSfc::k88200), "88.2 kHz"},
    NameEntry{static_cast<uint32_t>(CipSfc::k96000), "96 kHz"},
    NameEntry{static_cast<uint32_t>(CipSfc::k176400), "176.4 kHz"},
    NameEntry{static_cast<uint32_t>(CipSfc::k192000), "192 kHz"},
};

} // namespace names

[[nodiscard]] inline std::string Describe(StreamFormatRate value) {
    return DescribeValue(names::kStreamFormatRates, "stream_format_rate", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(CipSfc value) {
    return DescribeValue(names::kCipSfcs, "sfc", static_cast<uint32_t>(value), 2);
}

} // namespace ASFW::AVC

namespace ASFW::AVC::Cmd {

namespace names {

inline constexpr std::array kPlugDirections{
    NameEntry{static_cast<uint32_t>(PlugDirection::kInput), "input"},
    NameEntry{static_cast<uint32_t>(PlugDirection::kOutput), "output"},
};
inline constexpr std::array kPlugAddressModes{
    NameEntry{static_cast<uint32_t>(PlugAddressMode::kUnit), "unit"},
    NameEntry{static_cast<uint32_t>(PlugAddressMode::kSubunit), "subunit"},
    NameEntry{static_cast<uint32_t>(PlugAddressMode::kFunctionBlock), "function block"},
};
inline constexpr std::array kUnitPlugTypes{
    NameEntry{static_cast<uint32_t>(UnitPlugType::kPcr), "isochronous (PCR)"},
    NameEntry{static_cast<uint32_t>(UnitPlugType::kExternal), "external"},
    NameEntry{static_cast<uint32_t>(UnitPlugType::kAsync), "asynchronous"},
};
// ta1394 stream-format lib.rs:1080-1083.
inline constexpr std::array kSupportStatuses{
    NameEntry{static_cast<uint32_t>(SupportStatus::kActive), "active"},
    NameEntry{static_cast<uint32_t>(SupportStatus::kInactive), "inactive"},
    NameEntry{static_cast<uint32_t>(SupportStatus::kNoStreamFormat), "no stream format"},
    NameEntry{static_cast<uint32_t>(SupportStatus::kNotUsed), "not used"},
};
// TA 2001002 Table 5.1 and Table 5.4; the compound AM824 value is from the unpublished draft.
inline constexpr std::array kFormatRoots{
    NameEntry{0x80, "DVCR"},
    NameEntry{kFormatRootAm, "Audio & Music"},
    NameEntry{0xFF, "invalid"},
};
inline constexpr std::array kFormatLevel1AudioMusic{
    NameEntry{kFormatLevel1Am824, "AM824"},
    NameEntry{0x01, "24bit*4 audio pack"},
    NameEntry{0x02, "32bit floating point data"},
    NameEntry{kFormatLevel1CompoundAm824, "compound AM824 (draft)"},
    NameEntry{0xFF, "don't care"},
};
// TA 2001002 Table 5.5; 0E, 0F, 10 and 40 come from the draft (ta1394 lib.rs:379-392).
inline constexpr std::array kAm824Formats{
    NameEntry{static_cast<uint32_t>(Am824Format::kIec60958_3), "IEC 60958-3"},
    NameEntry{static_cast<uint32_t>(Am824Format::kIec61937_3), "IEC 61937-3"},
    NameEntry{static_cast<uint32_t>(Am824Format::kIec61937_4), "IEC 61937-4"},
    NameEntry{static_cast<uint32_t>(Am824Format::kIec61937_5), "IEC 61937-5"},
    NameEntry{static_cast<uint32_t>(Am824Format::kIec61937_6), "IEC 61937-6"},
    NameEntry{static_cast<uint32_t>(Am824Format::kIec61937_7), "IEC 61937-7"},
    NameEntry{static_cast<uint32_t>(Am824Format::kMultiBitLinearAudioRaw), "multi-bit linear audio (raw)"},
    NameEntry{static_cast<uint32_t>(Am824Format::kMultiBitLinearAudioDvd), "multi-bit linear audio (DVD-Audio)"},
    NameEntry{static_cast<uint32_t>(Am824Format::kOneBitAudioPlainRaw), "one bit audio (plain) raw"},
    NameEntry{static_cast<uint32_t>(Am824Format::kOneBitAudioPlainSacd), "one bit audio (plain) SACD"},
    NameEntry{static_cast<uint32_t>(Am824Format::kOneBitAudioEncodedRaw), "one bit audio (encoded) raw"},
    NameEntry{static_cast<uint32_t>(Am824Format::kOneBitAudioEncodedSacd), "one bit audio (encoded) SACD"},
    NameEntry{static_cast<uint32_t>(Am824Format::kHighPrecisionMultiBitLinearAudio), "high precision multi-bit linear audio"},
    NameEntry{static_cast<uint32_t>(Am824Format::kMidiConformant), "MIDI conformant"},
    NameEntry{static_cast<uint32_t>(Am824Format::kSmpteTimeCode), "SMPTE time code"},
    NameEntry{static_cast<uint32_t>(Am824Format::kSampleCount), "sample count"},
    NameEntry{static_cast<uint32_t>(Am824Format::kAncillaryData), "ancillary data"},
    NameEntry{static_cast<uint32_t>(Am824Format::kSyncStream), "sync stream"},
};
inline constexpr std::array kRateControls{
    NameEntry{static_cast<uint32_t>(RateControl::kSupported), "supported"},
    NameEntry{static_cast<uint32_t>(RateControl::kDontCare), "don't care"},
    NameEntry{static_cast<uint32_t>(RateControl::kNotSupported), "not supported"},
};
// TA 2004006 Figures 56, 57: the fmt byte is eoh | form | FMT; IEC 61883-6 Table 2 for FMT 0x10.
inline constexpr std::array kSignalFormatFmts{
    NameEntry{kFmtAm824, "audio and music (eoh 1, form 0, FMT 10)"},
    NameEntry{kFmtStatusWildcard, "wildcard (STATUS query)"},
};
// TA 1999008 Table 10.2.
inline constexpr std::array kFunctionBlockTypes{
    NameEntry{static_cast<uint32_t>(FunctionBlockType::kSelector), "Selector"},
    NameEntry{static_cast<uint32_t>(FunctionBlockType::kFeature), "Feature"},
    NameEntry{static_cast<uint32_t>(FunctionBlockType::kProcessing), "Processing"},
    NameEntry{static_cast<uint32_t>(FunctionBlockType::kCodec), "CODEC"},
};
// TA 1999008 §9.1.4 control_attribute.
inline constexpr std::array kControlAttributes{
    NameEntry{static_cast<uint32_t>(ControlAttribute::kResolution), "resolution"},
    NameEntry{static_cast<uint32_t>(ControlAttribute::kMinimum), "minimum"},
    NameEntry{static_cast<uint32_t>(ControlAttribute::kMaximum), "maximum"},
    NameEntry{static_cast<uint32_t>(ControlAttribute::kDefault), "default"},
    NameEntry{static_cast<uint32_t>(ControlAttribute::kDuration), "duration"},
    NameEntry{static_cast<uint32_t>(ControlAttribute::kCurrent), "current"},
    NameEntry{static_cast<uint32_t>(ControlAttribute::kMove), "move"},
    NameEntry{static_cast<uint32_t>(ControlAttribute::kDelta), "delta"},
};
// TA 1999008 §10.3 control selectors; ta1394 audio lib.rs:802-813.
inline constexpr std::array kFeatureControls{
    NameEntry{static_cast<uint32_t>(FeatureControl::kMute), "mute"},
    NameEntry{static_cast<uint32_t>(FeatureControl::kVolume), "volume"},
    NameEntry{static_cast<uint32_t>(FeatureControl::kLrBalance), "LR balance"},
    NameEntry{static_cast<uint32_t>(FeatureControl::kFrBalance), "FR balance"},
    NameEntry{static_cast<uint32_t>(FeatureControl::kBass), "bass"},
    NameEntry{static_cast<uint32_t>(FeatureControl::kMid), "mid"},
    NameEntry{static_cast<uint32_t>(FeatureControl::kTreble), "treble"},
    NameEntry{static_cast<uint32_t>(FeatureControl::kGraphicEqualizer), "graphic equalizer"},
    NameEntry{static_cast<uint32_t>(FeatureControl::kAutomaticGain), "automatic gain control"},
    NameEntry{static_cast<uint32_t>(FeatureControl::kDelay), "delay"},
    NameEntry{static_cast<uint32_t>(FeatureControl::kBassBoost), "bass boost"},
    NameEntry{static_cast<uint32_t>(FeatureControl::kLoudness), "loudness"},
};
// TA 2002013 §6.1 Table 14.
inline constexpr std::array kDescriptorSpecifierTypes{
    NameEntry{static_cast<uint32_t>(DescriptorSpecifierType::kUnitOrSubunitIdentifier), "(sub)unit identifier descriptor"},
    NameEntry{static_cast<uint32_t>(DescriptorSpecifierType::kListById), "list descriptor by list_ID"},
    NameEntry{static_cast<uint32_t>(DescriptorSpecifierType::kListByType), "list descriptor by list_type"},
    NameEntry{static_cast<uint32_t>(DescriptorSpecifierType::kSubunitDependentFirst), "subunit dependent descriptor"},
};
// TA 2002013 §7.4 (Table 30 context), §7.5.
inline constexpr std::array kReadResultStatuses{
    NameEntry{static_cast<uint32_t>(ReadResultStatus::kComplete), "complete read"},
    NameEntry{static_cast<uint32_t>(ReadResultStatus::kMoreToRead), "more to read"},
    NameEntry{static_cast<uint32_t>(ReadResultStatus::kDataLengthTooLarge), "data length too large"},
};
// TA 2002010 Table 7.7.
inline constexpr std::array kOutputStatuses{
    NameEntry{static_cast<uint32_t>(OutputStatus::kEffective), "effective"},
    NameEntry{static_cast<uint32_t>(OutputStatus::kNotEffective), "not effective"},
    NameEntry{static_cast<uint32_t>(OutputStatus::kInsufficientResource), "insufficient resource"},
    NameEntry{static_cast<uint32_t>(OutputStatus::kReady), "ready"},
    NameEntry{static_cast<uint32_t>(OutputStatus::kVirtualOutput), "virtual output"},
};
// TA 2002010 Table 7.6.
inline constexpr std::array kSignalSourceResults{
    NameEntry{static_cast<uint32_t>(SignalSourceResult::kSource), "source"},
    NameEntry{static_cast<uint32_t>(SignalSourceResult::kNotSource), "not source (through)"},
    NameEntry{static_cast<uint32_t>(SignalSourceResult::kNoInformation), "no information"},
};

} // namespace names

[[nodiscard]] inline std::string Describe(PlugDirection value) {
    return DescribeValue(names::kPlugDirections, "plug_direction", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(PlugAddressMode value) {
    return DescribeValue(names::kPlugAddressModes, "plug_address_mode", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(UnitPlugType value) {
    return DescribeValue(names::kUnitPlugTypes, "unit_plug_type", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(SupportStatus value) {
    return DescribeValue(names::kSupportStatuses, "support_status", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(Am824Format value) {
    return DescribeValue(names::kAm824Formats, "am824_format", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(RateControl value) {
    return DescribeValue(names::kRateControls, "rate_control", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(FunctionBlockType value) {
    return DescribeValue(names::kFunctionBlockTypes, "function_block_type", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(ControlAttribute value) {
    return DescribeValue(names::kControlAttributes, "control_attribute", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(FeatureControl value) {
    return DescribeValue(names::kFeatureControls, "control_selector", static_cast<uint32_t>(value), 2);
}
[[nodiscard]] inline std::string Describe(OutputStatus value) {
    return DescribeValue(names::kOutputStatuses, "output_status", static_cast<uint32_t>(value), 1);
}
[[nodiscard]] inline std::string Describe(SignalSourceResult value) {
    return DescribeValue(names::kSignalSourceResults, "result_status", static_cast<uint32_t>(value), 1);
}
[[nodiscard]] inline std::string Describe(ReadResultStatus value) {
    return DescribeValue(names::kReadResultStatuses, "read_result_status", static_cast<uint32_t>(value), 2);
}

/// "list descriptor by list_ID(0x10) [10 18 01]": the type by name, then the specifier bytes as sent.
[[nodiscard]] inline std::string Describe(const DescriptorSpecifier& specifier) {
    const auto bytes = specifier.Bytes();
    std::string text = bytes.empty() ? std::string("empty specifier")
                                     : DescribeValue(names::kDescriptorSpecifierTypes, "descriptor_specifier_type", bytes[0], 2);
    return text + " [" + HexBytes(bytes) + "]";
}

/// The fields of a compound AM824 format before its entries (TA 2001002 Table 5.4, compound AM824 draft).
[[nodiscard]] inline std::string DescribeCompoundHeader(const CompoundAm824& c) {
    return "compound AM824 rate=" + Describe(c.rate) + " sync_source=" + (c.syncSource ? "yes" : "no") +
           " rate_control=" + Describe(c.rateControl);
}
/// One entry of a compound AM824 format: "8 x multi-bit linear audio (raw)(0x06)".
[[nodiscard]] inline std::string Describe(const CompoundEntry& entry) {
    return std::to_string(entry.count) + " x " + Describe(entry.format);
}

/// A stream format block, field by field. Compound AM824 is decoded; any other block keeps its bytes.
[[nodiscard]] inline std::string Describe(const StreamFormat& format) {
    if (format.kind == StreamFormat::Kind::kCompoundAm824) {
        std::string text = DescribeCompoundHeader(format.compound) + " entries=[";
        bool first = true;
        for (const auto& entry : format.compound.Entries()) {
            if (!first) text += ", ";
            first = false;
            text += Describe(entry);
        }
        return text + "]";
    }
    const auto raw = format.Raw();
    std::string text = "format";
    if (!raw.empty()) {
        text += " root=" + DescribeValue(names::kFormatRoots, "format_root", raw[0], 2);
        if (raw.size() > 1) text += " level1=" + DescribeValue(names::kFormatLevel1AudioMusic, "format_level1", raw[1], 2);
    }
    return text + " bytes=[" + HexBytes(raw) + "]";  // no table describes it: the bytes are the only record
}

/// The fmt/fdf of an INPUT/OUTPUT PLUG SIGNAL FORMAT answer: the format by name, the SFC when it is AM824.
[[nodiscard]] inline std::string Describe(const PlugSignalFormat& format) {
    std::string text = "fmt=" + DescribeValue(names::kSignalFormatFmts, "fmt", format.fmt, 2) + " fdf=[" +
                       HexBytes(format.fdf) + "]";
    if (const auto sfc = SfcOf(format)) text += " sfc=" + Describe(*sfc);
    return text;
}

// ---------------------------------------------------------------------------
// CCM: signal addresses, the STATUS byte and its departures from the spec
// ---------------------------------------------------------------------------

/// Which end of a signal path an address names. The same plug number means a different plug at each
/// end: a unit input is an iPCR or external input plug, a unit output is an oPCR or external output plug.
enum class SignalRole : uint8_t { kSource, kDestination };

/// "iPCR[0]", "external output plug 1", "Music#0 (0x60) source plug 5", "no signal", ... with the bytes as sent.
[[nodiscard]] inline std::string Describe(const SignalAddress& address, SignalRole role) {
    const bool source = role == SignalRole::kSource;
    std::string text;
    switch (address.Kind()) {
        case SignalAddressKind::kNoSignal: text = source ? "no signal source" : "no signal destination"; break;
        case SignalAddressKind::kMultiple: text = "multiple"; break;
        case SignalAddressKind::kAvConvergent: text = "AV convergent"; break;
        case SignalAddressKind::kUnitPlug: {
            const auto plug = address.UnitPlug();
            if (plug.IsSerialBus()) {
                text = std::string(source ? "iPCR[" : "oPCR[") + std::to_string(*plug.Number()) + "]";
            } else if (plug.IsExternal()) {
                text = std::string("external ") + (source ? "input" : "output") + " plug " + std::to_string(*plug.Number());
            } else if (plug.Raw() == UnitPlugId::AnyAvailableSerialBus().Raw()) {
                text = "any available serial bus plug";
            } else if (plug.Raw() == UnitPlugId::AnyAvailableExternal().Raw()) {
                text = "any available external plug";
            } else {
                text = "UNKNOWN(unit_plug_id:" + Hex(plug.Raw()) + ")";
            }
            break;
        }
        case SignalAddressKind::kSubunitPlug:
            text = Describe(address.Subunit()) + (source ? " source plug " : " destination plug ") +
                   (address.bytes[1] == SignalAddress::kAnyAvailableSubunitPlugId ? std::string("any available")
                                                                                  : std::to_string(address.bytes[1]));
            break;
    }
    return text + " [" + HexBytes(address.bytes) + "]";
}

[[nodiscard]] inline std::string Describe(DestinationPlugKind kind) {
    switch (kind) {
        case DestinationPlugKind::kSerialBusOutputPlug: return "serial bus oPCR";
        case DestinationPlugKind::kExternalOutputPlug: return "external output plug";
        case DestinationPlugKind::kSubunitDestinationPlug: return "subunit destination plug";
    }
    return "UNKNOWN(destination_plug_kind:" + Hex(static_cast<uint32_t>(kind)) + ")";
}

/// signal_status: all clear is "identical"; otherwise the flags that are set (TA 2002010 Figure 10.7 / Fig 7.10).
[[nodiscard]] inline std::string Describe(SignalModifications modifications) {
    if (modifications.Identical()) return "identical(" + Hex(modifications.bits, 1) + ")";
    std::string text;
    const auto add = [&text](const char* flag) {
        if (!text.empty()) text += "+";
        text += flag;
    };
    if (modifications.Processed()) add("processed");
    if (modifications.Filtered()) add("filtered");
    if (modifications.Converted()) add("converted");
    if (modifications.OsdOverlaid()) add("OSD overlaid");
    return text + "(" + Hex(modifications.bits, 1) + ")";
}

/// "output_status=ready(0x3) conv=can change format(1) signal_status=identical(0x0)": the CCM STATUS byte, field by field.
[[nodiscard]] inline std::string Describe(SignalSourceStatusField status) {
    const std::string outputStatus =
        DescribeValue(names::kOutputStatuses, "output_status", status.OutputStatusCode(), 1);
    return "output_status=" + outputStatus + " conv=" + (status.CanChangeFormat() ? "can change format(1)" : "fixed format(0)") +
           " signal_status=" + Describe(status.Modifications());
}

/// The spec departures in a STATUS answer, in words; "none" when it conforms (TA 2002010 Tables 7.7-7.10).
[[nodiscard]] inline std::string Describe(StatusDeviations deviations) {
    if (!deviations.Any()) return "none";
    std::string text;
    const auto add = [&text](const char* what) {
        if (!text.empty()) text += "; ";
        text += what;
    };
    if (deviations.reservedOutputStatus) add("reserved output_status");
    if (deviations.outputStatusNotAllowedForPlug) add("output_status beyond effective/not effective");
    if (deviations.convSetOnNonSerialBusPlug) add("conv set on a plug that is not an oPCR");
    return text;
}

} // namespace ASFW::AVC::Cmd
