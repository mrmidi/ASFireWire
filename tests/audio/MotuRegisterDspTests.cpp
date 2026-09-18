// SPDX-License-Identifier: Apache-2.0
//
// MotuRegisterDsp tests: the output-volume encoding and the DSP message scan over capture
// data blocks (Linux motu-register-dsp-message-parser.c:160-170).
//
// The register is linear in AMPLITUDE (ALSA DB_LINEAR, alsa-ctl-tlv-codec items.rs:123-128),
// so the conversion is checked against std::log10/std::pow rather than a dB-per-step rule.
// Reading it as 0.5 dB per step is what made every level far too loud on hardware.

#include "Audio/Wire/MOTU/MotuRegisterDsp.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using ASFW::Encoding::Motu::kOutputVolumeMaxRaw;
using ASFW::Encoding::Motu::LatestDspMessageValue;
using ASFW::Encoding::Motu::OutputVolumeFromDb;
using ASFW::Encoding::Motu::OutputVolumeToDb;
using ASFW::Encoding::Motu::RegisterDspMessage;

constexpr uint32_t kPrefixBytes = 16; // isoch header + CIP header, as the consumer sees it
constexpr uint32_t kDbs = 13;         // 14 PCM chunks
constexpr uint32_t kBlockBytes = kDbs * 4;

[[nodiscard]] uint8_t Flag(RegisterDspMessage type, bool midi = false) {
    return static_cast<uint8_t>((static_cast<uint8_t>(type) << 3) | (midi ? 1U : 0U));
}

struct Payload {
    std::vector<uint8_t> bytes;
    explicit Payload(uint32_t blocks) : bytes(kPrefixBytes + blocks * kBlockBytes, 0) {}
    void SetMessage(uint32_t block, uint8_t flag, uint8_t value) {
        bytes[kPrefixBytes + block * kBlockBytes + 4] = flag;
        bytes[kPrefixBytes + block * kBlockBytes + 5] = value;
    }
};

TEST(MotuRegisterDspTests, OutputVolumeIsLinearInAmplitude) {
    EXPECT_NEAR(OutputVolumeToDb(kOutputVolumeMaxRaw), 0.0f, 0.001f);   // unity
    EXPECT_NEAR(OutputVolumeToDb(0x40), -6.0206f, 0.01f);               // half amplitude
    EXPECT_NEAR(OutputVolumeToDb(0x20), -12.041f, 0.01f);
    EXPECT_NEAR(OutputVolumeToDb(0x08), -24.082f, 0.01f);               // NOT -60 dB
    EXPECT_NEAR(OutputVolumeToDb(1), ASFW::Encoding::Motu::kOutputVolumeMinDb, 0.01f);
    // Raw 0 is off; the published floor stands in for -inf.
    EXPECT_FLOAT_EQ(OutputVolumeToDb(0), ASFW::Encoding::Motu::kOutputVolumeMinDb);
    // Out-of-range raw values clamp rather than reading as a positive gain.
    EXPECT_NEAR(OutputVolumeToDb(0xFF), 0.0f, 0.001f);
}

TEST(MotuRegisterDspTests, ConversionsMatchTheReferenceFormulaWithoutLibm) {
    // dB = 20 * log10(raw / 128), the DB_LINEAR definition.
    for (uint32_t raw = 1; raw <= kOutputVolumeMaxRaw; ++raw) {
        const float expected =
            20.0f * std::log10(static_cast<float>(raw) / static_cast<float>(kOutputVolumeMaxRaw));
        EXPECT_NEAR(OutputVolumeToDb(static_cast<uint8_t>(raw)), expected, 0.01f) << raw;
    }
    for (float db = -42.0f; db <= 0.0f; db += 0.25f) {
        const auto expected =
            static_cast<uint32_t>(std::lround(128.0 * std::pow(10.0, db / 20.0)));
        EXPECT_NEAR(OutputVolumeFromDb(db), expected, 1) << db;
    }
}

TEST(MotuRegisterDspTests, OutputVolumeRoundTripsEveryAudibleStep) {
    for (uint32_t raw = 1; raw <= kOutputVolumeMaxRaw; ++raw) {
        EXPECT_EQ(OutputVolumeFromDb(OutputVolumeToDb(static_cast<uint8_t>(raw))), raw) << raw;
    }
}

TEST(MotuRegisterDspTests, OutputVolumeFromDbClampsAndSilences) {
    EXPECT_EQ(OutputVolumeFromDb(0.0f), kOutputVolumeMaxRaw);
    EXPECT_EQ(OutputVolumeFromDb(6.0f), kOutputVolumeMaxRaw);
    EXPECT_EQ(OutputVolumeFromDb(-6.0206f), 0x40);
    // Below the last step the register goes off, which is how mute reaches true silence.
    EXPECT_EQ(OutputVolumeFromDb(-60.0f), 0);
    EXPECT_EQ(OutputVolumeFromDb(-144.0f), 0);
    EXPECT_EQ(OutputVolumeFromDb(std::numeric_limits<float>::quiet_NaN()), 0);
}

TEST(MotuRegisterDspTests, ScanReturnsTheLatestValueOfTheRequestedType) {
    Payload p{4};
    p.SetMessage(0, Flag(RegisterDspMessage::kMainOutputVolume), 0x50);
    p.SetMessage(1, Flag(RegisterDspMessage::kPhonesVolume), 0x10);
    // The MIDI flag shares the byte and must not hide the type.
    p.SetMessage(2, Flag(RegisterDspMessage::kMainOutputVolume, /*midi=*/true), 0x60);

    const auto main = LatestDspMessageValue(p.bytes, kDbs, 4, kPrefixBytes,
                                            RegisterDspMessage::kMainOutputVolume);
    ASSERT_TRUE(main.has_value());
    EXPECT_EQ(*main, 0x60);

    const auto phones = LatestDspMessageValue(p.bytes, kDbs, 4, kPrefixBytes,
                                              RegisterDspMessage::kPhonesVolume);
    ASSERT_TRUE(phones.has_value());
    EXPECT_EQ(*phones, 0x10);
}

TEST(MotuRegisterDspTests, ScanFindsNothingWhenNoBlockCarriesTheType) {
    Payload p{2};
    p.SetMessage(0, Flag(RegisterDspMessage::kPhonesVolume), 0x10);
    EXPECT_FALSE(LatestDspMessageValue(p.bytes, kDbs, 2, kPrefixBytes,
                                       RegisterDspMessage::kMainOutputVolume)
                     .has_value());
}

TEST(MotuRegisterDspTests, ScanNeverReadsPastAShortPayload) {
    Payload p{2};
    p.SetMessage(1, Flag(RegisterDspMessage::kMainOutputVolume), 0x70);
    p.bytes.resize(kPrefixBytes + kBlockBytes + 3); // second block truncated
    EXPECT_FALSE(LatestDspMessageValue(p.bytes, kDbs, 2, kPrefixBytes,
                                       RegisterDspMessage::kMainOutputVolume)
                     .has_value());
}

} // namespace
