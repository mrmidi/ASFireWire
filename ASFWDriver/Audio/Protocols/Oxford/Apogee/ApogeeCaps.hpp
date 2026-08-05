// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeCaps.hpp - Apogee Duet FireWire device spec: control capabilities and
// streaming quirks, in one constexpr place (FW-130).
//
// A device spec is not only control ranges. The streaming quirks the kernel
// tracks per model belong here too, because both describe "what this device
// is" rather than "how we talk to it". Neither reference alone is sufficient:
//
//   * control ranges  - snd-firewire-ctl-services protocols/oxfw/src/apogee.rs
//   * streaming quirks - linux .../firewire/oxfw/oxfw.c + oxfw.h
//
// Both are behavioural sources, read only; no code is copied from either.
//
// The Duet is an OXFW971 (oxfw.c:377). Linux documents 971 SYT as reliable
// with both transmission modes available, unlike the 970 group where per-device
// comments record SYT as unreliable or pinned to 0xffff.

#pragma once

#include <array>
#include <cstdint>

namespace ASFW::Audio::Oxford::Apogee {

//==============================================================================
// Range primitive
//==============================================================================

/// Declaring a malformed range is a compile error rather than a runtime
/// surprise. This is deliberately never defined: constant evaluation of the
/// guarded branch then has no definition to call, and the translation unit is
/// rejected with "undefined function ... cannot be used in a constant
/// expression". The missing body is the mechanism, so the warning about it is
/// suppressed here rather than fixed.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wundefined-inline"
consteval void ControlRangeMustBeOrdered();
#pragma clang diagnostic pop

template <typename T>
struct ControlRange {
    T min{};
    T max{};

    consteval ControlRange(T lo, T hi) : min(lo), max(hi) {
        if (!(lo < hi)) {
            ControlRangeMustBeOrdered();
        }
    }

    [[nodiscard]] constexpr bool Contains(T value) const noexcept {
        return value >= min && value <= max;
    }

    [[nodiscard]] constexpr T Clamp(T value) const noexcept {
        if (value < min) {
            return min;
        }
        if (value > max) {
            return max;
        }
        return value;
    }

    friend constexpr bool operator==(const ControlRange&, const ControlRange&) = default;
};

//==============================================================================
// How a raw input-gain value should be read as dB
//==============================================================================

/// The same raw 10..75 gain means different dB depending on which input is
/// selected, so the interpretation needs source *and* nominal level together —
/// neither alone determines it (apogee.rs:465-494, :653-655).
enum class InputGainContext : uint8_t {
    /// XLR at microphone level: dB == raw, 10..75 dB.
    kXlrMicrophone = 0,
    /// Phone plug: 0..65 dB, i.e. dB == raw - 10.
    kPhone,
    /// XLR at Professional (+4 dBu) or Consumer (-10 dBV): fixed gain, so the
    /// raw value is inert and no dB reading is meaningful.
    kXlrFixedLevel,
};

//==============================================================================
// The spec
//==============================================================================

struct ApogeeDuetSpec {
    //--------------------------------------------------------------------------
    // Identity (apogee.rs:127, :871)
    //--------------------------------------------------------------------------

    static constexpr std::array<uint8_t, 3> kOui{0x00, 0x03, 0xDB};
    static constexpr std::array<uint8_t, 3> kMagic{'P', 'C', 'M'};

    // Deliberately absent: no volume/mute Feature Block ids. The Duet replaces
    // the standard AV/C Feature Block volume/mute path with vendor commands
    // (OutVolume 0x15, OutMute 0x09) - only 2 of the 5 reference OXFW devices
    // use Feature Blocks, which is why the OXFW-common layer cannot assume them.

    //--------------------------------------------------------------------------
    // Control ranges (apogee.rs)
    //--------------------------------------------------------------------------

    /// Output volume, 1 dB per step (:456-461).
    static constexpr ControlRange<uint8_t> kOutputVolume{0, 64};
    /// dB at the endpoints of kOutputVolume (:298-301).
    static constexpr int kOutputVolumeMinDb = -64;
    static constexpr int kOutputVolumeMaxDb = 0;

    /// Raw input gain (:653-655). Its dB meaning depends on InputGainContext.
    static constexpr ControlRange<uint8_t> kInputGain{10, 75};

    /// Mixer source gain, a linear coefficient (:724-726).
    static constexpr ControlRange<uint16_t> kMixerSourceGain{0, 0x3FFF};

    /// Meter level (:140-148).
    static constexpr ControlRange<int32_t> kMeterLevel{0, 0x7FFFFFFF};
    static constexpr int32_t kMeterLevelStep = 0x100;

    //--------------------------------------------------------------------------
    // Streaming quirks (oxfw.c:164-167, flags at oxfw.h:35-60)
    //--------------------------------------------------------------------------

    /// SND_OXFW_QUIRK_BLOCKING_TRANSMISSION. Already honoured as
    /// StreamMode::kBlocking in ApogeeDuetProfile.cpp; recorded here so the
    /// fact has a citation rather than living unsourced in a profile.
    static constexpr bool kBlockingTransmission = true;

    /// SND_OXFW_QUIRK_IGNORE_NO_INFO_PACKET. Per the kernel, the Duet skips
    /// data blocks in NO_INFO packets when processing audio (though its output
    /// level meter still moves), and will process audio for any syt value at
    /// all - including ones IEC 61883-1/6 considers invalid.
    ///
    /// UNVERIFIED against our stack. Recorded as a device fact only; nothing
    /// keys off it yet, and confirming it needs hardware (FW-140).
    static constexpr bool kIgnoresNoInfoPackets = true;

    //--------------------------------------------------------------------------
    // Conversions
    //--------------------------------------------------------------------------

    /// Output volume is a plain 1 dB/step ladder: raw 0 => -64 dB, raw 64 => 0 dB.
    [[nodiscard]] static constexpr int OutputVolumeToDb(uint8_t raw) noexcept {
        return static_cast<int>(kOutputVolume.Clamp(raw)) + kOutputVolumeMinDb;
    }

    [[nodiscard]] static constexpr uint8_t OutputVolumeFromDb(int db) noexcept {
        if (db <= kOutputVolumeMinDb) {
            return kOutputVolume.min;
        }
        if (db >= kOutputVolumeMaxDb) {
            return kOutputVolume.max;
        }
        return static_cast<uint8_t>(db - kOutputVolumeMinDb);
    }

    /// dB range a raw input gain spans in the given context. Fixed-level inputs
    /// report an empty range: the gain value carries no dB meaning there, and
    /// presenting one would be a fabrication.
    [[nodiscard]] static constexpr bool InputGainHasDbMeaning(InputGainContext ctx) noexcept {
        return ctx != InputGainContext::kXlrFixedLevel;
    }

    [[nodiscard]] static constexpr int InputGainToDb(uint8_t raw, InputGainContext ctx) noexcept {
        const int clamped = static_cast<int>(kInputGain.Clamp(raw));
        switch (ctx) {
            case InputGainContext::kXlrMicrophone:
                return clamped;  // 10..75 dB
            case InputGainContext::kPhone:
                return clamped - static_cast<int>(kInputGain.min);  // 0..65 dB
            case InputGainContext::kXlrFixedLevel:
            default:
                return 0;
        }
    }
};

//==============================================================================
// Invariants
//==============================================================================

static_assert(ApogeeDuetSpec::kOutputVolume.min < ApogeeDuetSpec::kOutputVolume.max);
static_assert(ApogeeDuetSpec::kInputGain.min < ApogeeDuetSpec::kInputGain.max);
static_assert(ApogeeDuetSpec::kMixerSourceGain.min < ApogeeDuetSpec::kMixerSourceGain.max);
static_assert(ApogeeDuetSpec::kMeterLevel.min < ApogeeDuetSpec::kMeterLevel.max);

// The dB ladder must line up with the raw endpoints, or a volume slider would
// silently misreport at one end.
static_assert(ApogeeDuetSpec::kOutputVolumeMaxDb - ApogeeDuetSpec::kOutputVolumeMinDb ==
                  static_cast<int>(ApogeeDuetSpec::kOutputVolume.max) -
                      static_cast<int>(ApogeeDuetSpec::kOutputVolume.min),
              "output volume dB span must match its raw span at 1 dB per step");
static_assert(ApogeeDuetSpec::OutputVolumeToDb(ApogeeDuetSpec::kOutputVolume.min) ==
              ApogeeDuetSpec::kOutputVolumeMinDb);
static_assert(ApogeeDuetSpec::OutputVolumeToDb(ApogeeDuetSpec::kOutputVolume.max) ==
              ApogeeDuetSpec::kOutputVolumeMaxDb);

// Phone spans 0..65 dB over the same raw domain as XLR microphone's 10..75.
static_assert(ApogeeDuetSpec::InputGainToDb(ApogeeDuetSpec::kInputGain.min,
                                            InputGainContext::kPhone) == 0);
static_assert(ApogeeDuetSpec::InputGainToDb(ApogeeDuetSpec::kInputGain.max,
                                            InputGainContext::kPhone) == 65);
static_assert(ApogeeDuetSpec::InputGainToDb(ApogeeDuetSpec::kInputGain.min,
                                            InputGainContext::kXlrMicrophone) == 10);
static_assert(ApogeeDuetSpec::InputGainToDb(ApogeeDuetSpec::kInputGain.max,
                                            InputGainContext::kXlrMicrophone) == 75);

} // namespace ASFW::Audio::Oxford::Apogee
