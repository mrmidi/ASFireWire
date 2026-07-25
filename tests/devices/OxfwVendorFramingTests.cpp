// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OxfwVendorFramingTests.cpp - OXFW-common VENDOR-DEPENDENT framing (FW-126).
//
// The point of the template is that it carries no Apogee constants. That is
// proved here by instantiating it against a second, synthetic spec.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeVendorCodec.hpp"
#include "ASFWDriver/Audio/Protocols/Oxford/OxfwVendorDependent.hpp"

#include <vector>

namespace {

namespace Oxford = ASFW::Audio::Oxford;
using ASFW::Audio::Oxford::Apogee::ApogeeDuetSpec;
using ASFW::Audio::Oxford::Apogee::ApogeeFraming;
using ASFW::Audio::Oxford::Apogee::ApogeeVendorCommand;
using Code = ApogeeVendorCommand::Code;

// A second vendor, shaped like Tascam FireOne (tascam.rs:10, :249) but declared
// here so the test does not depend on a device we do not support.
struct SyntheticSpec {
    static constexpr std::array<uint8_t, 3> kOui{0x00, 0x02, 0x2E};
    static constexpr std::array<uint8_t, 3> kMagic{'F', 'I', '1'};
};

// A vendor that answers control with IMPLEMENTED/STABLE and echoes a bogus
// company_id, as FireOne does (tascam.rs:405-406). Proves the policy hooks are
// real rather than decorative.
struct QuirkySpec {
    static constexpr std::array<uint8_t, 3> kOui{0x00, 0x02, 0x2E};
    static constexpr std::array<uint8_t, 3> kMagic{'F', 'I', '1'};
    static constexpr bool kEchoesOuiInResponse = false;
    static constexpr bool AcceptsResponseCode(uint8_t code, bool) noexcept {
        return code == Oxford::kResponseImplementedStable;
    }
};

using SyntheticFraming = Oxford::VendorDependentFraming<SyntheticSpec>;
using QuirkyFraming = Oxford::VendorDependentFraming<QuirkySpec>;

// ============================================================================
// The template is generic
// ============================================================================

TEST(OxfwVendorFraming, WritesTheSpecsOwnPrefix) {
    std::vector<uint8_t> apogee;
    ApogeeFraming::WritePrefix(apogee, 0x15);
    EXPECT_EQ(apogee, (std::vector<uint8_t>{0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x15}));

    std::vector<uint8_t> synthetic;
    SyntheticFraming::WritePrefix(synthetic, 0x10);
    EXPECT_EQ(synthetic, (std::vector<uint8_t>{0x00, 0x02, 0x2E, 'F', 'I', '1', 0x10}));
}

TEST(OxfwVendorFraming, PrefixIsSevenBytesForEveryVendor) {
    // OUI(3) + magic(3) + code(1). Apogee's two argument bytes are *not* part
    // of this: FireOne puts a single operand straight after the code, so its
    // frames are 8 bytes where Apogee's header alone is 9.
    static_assert(Oxford::kVendorPrefixSize == 7U);
    static_assert(ApogeeFraming::kPrefixSize == SyntheticFraming::kPrefixSize);
    static_assert(ASFW::Audio::Oxford::Apogee::kApogeeHeaderSize == 9U);
    SUCCEED();
}

TEST(OxfwVendorFraming, ValidatesAgainstItsOwnVendorOnly) {
    const std::vector<uint8_t> apogeeResponse{0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x15, 0x80, 0xFF};
    EXPECT_TRUE(ApogeeFraming::ValidatePrefix(apogeeResponse, 0x15));
    // The same bytes must not satisfy a different vendor's framing.
    EXPECT_FALSE(SyntheticFraming::ValidatePrefix(apogeeResponse, 0x15));
}

// ============================================================================
// Structural rejection
// ============================================================================

TEST(OxfwVendorFraming, RejectsMalformedPrefixes) {
    const std::vector<uint8_t> good{0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x15, 0x80, 0xFF};
    ASSERT_TRUE(ApogeeFraming::ValidatePrefix(good, 0x15));

    auto wrongOui = good;
    wrongOui[1] = 0xFF;
    EXPECT_FALSE(ApogeeFraming::ValidatePrefix(wrongOui, 0x15));

    auto wrongMagic = good;
    wrongMagic[4] = 'X';
    EXPECT_FALSE(ApogeeFraming::ValidatePrefix(wrongMagic, 0x15));

    auto wrongCode = good;
    wrongCode[6] = 0x09;
    EXPECT_FALSE(ApogeeFraming::ValidatePrefix(wrongCode, 0x15));

    const std::vector<uint8_t> tooShort{0x00, 0x03, 0xDB, 'P', 'C'};
    EXPECT_FALSE(ApogeeFraming::ValidatePrefix(tooShort, 0x15));
}

// ============================================================================
// Response policy is overridable per spec
// ============================================================================

TEST(OxfwVendorFraming, DefaultResponsePolicyFollowsTheAvcSpec) {
    // CONTROL -> ACCEPTED, STATUS -> IMPLEMENTED/STABLE (oxfw-spkr.c:39-45).
    using Policy = Oxford::VendorResponsePolicy<ApogeeDuetSpec>;
    EXPECT_TRUE(Policy::AcceptsResponseCode(0x09, /*isStatus=*/false));
    EXPECT_FALSE(Policy::AcceptsResponseCode(0x0C, /*isStatus=*/false));
    EXPECT_TRUE(Policy::AcceptsResponseCode(0x0C, /*isStatus=*/true));
    EXPECT_TRUE(Policy::EchoesOui());
}

TEST(OxfwVendorFraming, SpecCanOverrideResponseAcceptance) {
    using Policy = Oxford::VendorResponsePolicy<QuirkySpec>;
    // Accepts IMPLEMENTED/STABLE even for control - the FireOne quirk.
    EXPECT_TRUE(Policy::AcceptsResponseCode(0x0C, /*isStatus=*/false));
    EXPECT_FALSE(Policy::AcceptsResponseCode(0x09, /*isStatus=*/false));
    EXPECT_FALSE(Policy::EchoesOui());
}

TEST(OxfwVendorFraming, OuiCheckIsSkippedWhenTheSpecSaysItIsNotEchoed) {
    // company_id 0xffffff in the response instead of the vendor's own OUI.
    const std::vector<uint8_t> quirkyResponse{0xFF, 0xFF, 0xFF, 'F', 'I', '1', 0x10};
    EXPECT_TRUE(QuirkyFraming::ValidatePrefix(quirkyResponse, 0x10));
    EXPECT_FALSE(SyntheticFraming::ValidatePrefix(quirkyResponse, 0x10));
}

// ============================================================================
// AV/C boolean operand encoding
// ============================================================================

TEST(OxfwVendorFraming, BooleanOperandUsesTheStandardAvcMuteEncoding) {
    // 0x70/0x60 is the AV/C Feature Block mute encoding (oxfw-spkr.c:58) that
    // Apogee reused for its vendor booleans - not an arbitrary constant.
    static_assert(Oxford::ToAvcBool(true) == 0x70);
    static_assert(Oxford::ToAvcBool(false) == 0x60);
    static_assert(Oxford::FromAvcBool(0x70));
    static_assert(!Oxford::FromAvcBool(0x60));
    // Anything that is not the ON pattern reads as off, rather than as true.
    EXPECT_FALSE(Oxford::FromAvcBool(0x00));
    EXPECT_FALSE(Oxford::FromAvcBool(0xFF));
}

// ============================================================================
// The Apogee codec sits on top of the template
// ============================================================================

TEST(ApogeeVendorCodec, EmitsSpecOuiAndMagicThroughTheTemplate) {
    const auto cmd = ApogeeVendorCommand::OutVolume(32);
    const auto operands = cmd.BuildOperandBase();
    ASSERT_EQ(operands.size(), 9U);
    EXPECT_EQ(operands[0], ApogeeDuetSpec::kOui[0]);
    EXPECT_EQ(operands[1], ApogeeDuetSpec::kOui[1]);
    EXPECT_EQ(operands[2], ApogeeDuetSpec::kOui[2]);
    EXPECT_EQ(operands[3], ApogeeDuetSpec::kMagic[0]);
    EXPECT_EQ(operands[6], static_cast<uint8_t>(Code::OutVolume));
}

TEST(ApogeeVendorCodec, MixerSrcSplitsGainBigEndian) {
    auto cmd = ApogeeVendorCommand::MixerSrc(/*source=*/2, /*destination=*/1, /*gain=*/0x1234);
    auto operands = cmd.BuildOperandBase();
    cmd.AppendControlValue(operands);
    ASSERT_EQ(operands.size(), 11U);
    EXPECT_EQ(operands[7], 0x10);  // EncodeMixerSource(2)
    EXPECT_EQ(operands[8], 0x01);  // destination
    EXPECT_EQ(operands[9], 0x12);  // gain high byte first
    EXPECT_EQ(operands[10], 0x34);
}

TEST(ApogeeVendorCodec, RejectsStatusPayloadsThatAreNotOurs) {
    auto cmd = ApogeeVendorCommand::OutVolume(0);
    const std::vector<uint8_t> good{0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x15, 0x80, 0xFF, 40};
    ASSERT_TRUE(cmd.ParseStatusPayload(good));
    EXPECT_EQ(cmd.u8Value, 40);

    auto wrongOui = good;
    wrongOui[0] = 0x01;
    EXPECT_FALSE(cmd.ParseStatusPayload(wrongOui));

    auto wrongPrefix = good;
    wrongPrefix[3] = 'Q';
    EXPECT_FALSE(cmd.ParseStatusPayload(wrongPrefix));

    auto wrongCode = good;
    wrongCode[6] = 0x09;
    EXPECT_FALSE(cmd.ParseStatusPayload(wrongCode));

    EXPECT_FALSE(cmd.ParseStatusPayload(std::vector<uint8_t>{0x00, 0x03, 0xDB}));

    // Header present but the value byte missing.
    EXPECT_FALSE(cmd.ParseStatusPayload(
        std::vector<uint8_t>{0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x15, 0x80, 0xFF}));

    // A rejected parse must leave the previous value intact, not half-written.
    EXPECT_EQ(cmd.u8Value, 40);
}

TEST(ApogeeVendorCodec, IndexedCommandRejectsAResponseForAnotherChannel) {
    auto cmd = ApogeeVendorCommand::IndexedBool(Code::MicPhantom, /*index=*/1, false);
    std::vector<uint8_t> other{0x00, 0x03, 0xDB, 'P', 'C', 'M',
                               static_cast<uint8_t>(Code::MicPhantom), 0x80, 0x00, 0x70};
    EXPECT_FALSE(cmd.ParseStatusPayload(other)) << "channel 0 answer must not fill channel 1";

    other[8] = 0x01;
    EXPECT_TRUE(cmd.ParseStatusPayload(other));
    EXPECT_TRUE(cmd.boolValue);
}

} // namespace
