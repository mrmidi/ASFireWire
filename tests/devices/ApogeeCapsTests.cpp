// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeCapsTests.cpp - Apogee Duet device spec (FW-130).
//
// Most invariants are static_asserts inside ApogeeCaps.hpp, so this file
// failing to compile is itself a failure signal. What is tested here is the
// behaviour those asserts cannot cover: full-domain round-trips, clamping, and
// the source-dependent reading of input gain.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeCaps.hpp"
#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeTypes.hpp"

namespace {

using ASFW::Audio::Oxford::Apogee::ApogeeDuetSpec;
using ASFW::Audio::Oxford::Apogee::ControlRange;
using ASFW::Audio::Oxford::Apogee::InputGainContext;

// ============================================================================
// Identity
// ============================================================================

TEST(ApogeeCaps, CarriesTheApogeeOuiAndVendorMagic) {
    // apogee.rs:127 and :871.
    constexpr std::array<uint8_t, 3> expectedOui{0x00, 0x03, 0xDB};
    constexpr std::array<uint8_t, 3> expectedMagic{'P', 'C', 'M'};
    static_assert(ApogeeDuetSpec::kOui == expectedOui);
    static_assert(ApogeeDuetSpec::kMagic == expectedMagic);
    EXPECT_EQ(ApogeeDuetSpec::kMagic[0], 0x50);  // 'P'
    EXPECT_EQ(ApogeeDuetSpec::kMagic[1], 0x43);  // 'C'
    EXPECT_EQ(ApogeeDuetSpec::kMagic[2], 0x4D);  // 'M'
}

// ============================================================================
// Output volume: raw 0..64 <=> -64..0 dB, 1 dB/step
// ============================================================================

TEST(ApogeeCaps, OutputVolumeRoundTripsAcrossTheWholeDomain) {
    for (int raw = ApogeeDuetSpec::kOutputVolume.min; raw <= ApogeeDuetSpec::kOutputVolume.max;
         ++raw) {
        const int db = ApogeeDuetSpec::OutputVolumeToDb(static_cast<uint8_t>(raw));
        EXPECT_EQ(db, raw - 64) << "raw " << raw;
        EXPECT_EQ(ApogeeDuetSpec::OutputVolumeFromDb(db), raw) << "dB " << db;
    }
}

TEST(ApogeeCaps, OutputVolumeEndpointsMatchTheReference) {
    EXPECT_EQ(ApogeeDuetSpec::OutputVolumeToDb(0), -64);
    EXPECT_EQ(ApogeeDuetSpec::OutputVolumeToDb(64), 0);
    EXPECT_EQ(ApogeeDuetSpec::OutputVolumeFromDb(-64), 0);
    EXPECT_EQ(ApogeeDuetSpec::OutputVolumeFromDb(0), 64);
}

TEST(ApogeeCaps, OutputVolumeClampsOutOfRangeInputs) {
    // Raw values above the domain saturate rather than wrapping into a loud
    // setting, which is the failure mode that matters for a volume control.
    EXPECT_EQ(ApogeeDuetSpec::OutputVolumeToDb(200), 0);
    EXPECT_EQ(ApogeeDuetSpec::OutputVolumeFromDb(-1000), 0);
    EXPECT_EQ(ApogeeDuetSpec::OutputVolumeFromDb(1000), 64);
}

// ============================================================================
// Input gain: same raw domain, different dB meaning per source
// ============================================================================

TEST(ApogeeCaps, InputGainDbMeaningShiftsWithTheSource) {
    // apogee.rs:465-494 - XLR + Microphone is 10..75 dB, Phone is 0..65 dB,
    // over the identical raw 10..75 domain.
    EXPECT_EQ(ApogeeDuetSpec::InputGainToDb(10, InputGainContext::kXlrMicrophone), 10);
    EXPECT_EQ(ApogeeDuetSpec::InputGainToDb(75, InputGainContext::kXlrMicrophone), 75);
    EXPECT_EQ(ApogeeDuetSpec::InputGainToDb(10, InputGainContext::kPhone), 0);
    EXPECT_EQ(ApogeeDuetSpec::InputGainToDb(75, InputGainContext::kPhone), 65);
}

TEST(ApogeeCaps, FixedLevelInputsReportNoDbMeaning) {
    // Professional (+4 dBu) and Consumer (-10 dBV) are fixed-gain: the raw
    // value is inert, so presenting a dB reading would be a fabrication.
    EXPECT_FALSE(ApogeeDuetSpec::InputGainHasDbMeaning(InputGainContext::kXlrFixedLevel));
    EXPECT_TRUE(ApogeeDuetSpec::InputGainHasDbMeaning(InputGainContext::kXlrMicrophone));
    EXPECT_TRUE(ApogeeDuetSpec::InputGainHasDbMeaning(InputGainContext::kPhone));
}

TEST(ApogeeCaps, InputGainClampsBelowTheDomain) {
    EXPECT_EQ(ApogeeDuetSpec::InputGainToDb(0, InputGainContext::kPhone), 0);
    EXPECT_EQ(ApogeeDuetSpec::InputGainToDb(255, InputGainContext::kPhone), 65);
}

// ============================================================================
// Remaining ranges
// ============================================================================

TEST(ApogeeCaps, MixerAndMeterRangesMatchTheReference) {
    static_assert(ApogeeDuetSpec::kMixerSourceGain.max == 0x3FFF);   // apogee.rs:724-726
    static_assert(ApogeeDuetSpec::kMeterLevel.max == 0x7FFFFFFF);    // i32::MAX, :140-148
    static_assert(ApogeeDuetSpec::kMeterLevelStep == 0x100);
    EXPECT_TRUE(ApogeeDuetSpec::kMixerSourceGain.Contains(0x3FFF));
    EXPECT_FALSE(ApogeeDuetSpec::kMixerSourceGain.Contains(0x4000));
}

// ============================================================================
// The spec is the single source: no duplicated literals left behind
// ============================================================================

TEST(ApogeeCaps, TypeConstantsResolveToTheSpec) {
    using ASFW::Audio::Oxford::Apogee::InputParams;
    using ASFW::Audio::Oxford::Apogee::InputMeterState;
    using ASFW::Audio::Oxford::Apogee::KnobState;
    using ASFW::Audio::Oxford::Apogee::MixerParams;
    using ASFW::Audio::Oxford::Apogee::OutputParams;

    // Before FW-130 these were five independent literal copies; a spec change
    // that failed to reach one of them would go unnoticed.
    static_assert(KnobState::kOutputVolMin == ApogeeDuetSpec::kOutputVolume.min);
    static_assert(KnobState::kOutputVolMax == ApogeeDuetSpec::kOutputVolume.max);
    static_assert(OutputParams::kVolumeMin == ApogeeDuetSpec::kOutputVolume.min);
    static_assert(OutputParams::kVolumeMax == ApogeeDuetSpec::kOutputVolume.max);
    static_assert(KnobState::kInputGainMin == ApogeeDuetSpec::kInputGain.min);
    static_assert(InputParams::kGainMin == ApogeeDuetSpec::kInputGain.min);
    static_assert(InputParams::kGainMax == ApogeeDuetSpec::kInputGain.max);
    static_assert(MixerParams::kGainMax == ApogeeDuetSpec::kMixerSourceGain.max);
    static_assert(InputMeterState::kStep == ApogeeDuetSpec::kMeterLevelStep);
    SUCCEED();
}

// ============================================================================
// Streaming quirks
// ============================================================================

TEST(ApogeeCaps, RecordsTheKernelStreamingQuirks) {
    // oxfw.c:164-167 gives the Duet BLOCKING_TRANSMISSION | IGNORE_NO_INFO_PACKET.
    static_assert(ApogeeDuetSpec::kBlockingTransmission);
    static_assert(ApogeeDuetSpec::kIgnoresNoInfoPackets);
    // kIgnoresNoInfoPackets is recorded but unverified against our stack, and
    // nothing keys off it yet - confirming it needs hardware (FW-140).
    SUCCEED();
}

// ============================================================================
// Malformed spec must not compile
// ============================================================================

TEST(ApogeeCaps, WellFormedRangesCompile) {
    static constexpr ControlRange<uint8_t> ordered{0, 64};
    static_assert(ordered.min < ordered.max);
    EXPECT_EQ(ordered.Clamp(200), 64);
}

// A malformed range is rejected at compile time by ControlRange's consteval
// constructor. Uncommenting either line below must fail the build with:
//
//   error: constexpr variable '...' must be initialized by a constant expression
//   note: undefined function 'ControlRangeMustBeOrdered' cannot be used in a
//         constant expression
//
// Verified with clang -std=gnu++23 -fno-exceptions -fno-rtti. It cannot be
// asserted positively from inside a passing translation unit, because a
// translation unit that contains it does not compile.
//
//   static constexpr ControlRange<uint8_t> equalBounds{64, 64};
//   static constexpr ControlRange<uint8_t> invertedBounds{64, 0};

} // namespace
