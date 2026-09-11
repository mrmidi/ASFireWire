// SPDX-License-Identifier: Apache-2.0
//
// MotuRegisterDsp tests: the output-volume encoding (0..0x80 <-> -64..0 dB, ctl-services
// register_dsp_ctls.rs:841-846) and the DSP message scan over capture data blocks (Linux
// motu-register-dsp-message-parser.c:160-170).

#include "Audio/Wire/MOTU/MotuRegisterDsp.hpp"

#include <gtest/gtest.h>

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

TEST(MotuRegisterDspTests, OutputVolumeSpansMinus64To0DbInHalfDbSteps) {
    EXPECT_FLOAT_EQ(OutputVolumeToDb(kOutputVolumeMaxRaw), 0.0f);
    EXPECT_FLOAT_EQ(OutputVolumeToDb(0), -64.0f);
    EXPECT_FLOAT_EQ(OutputVolumeToDb(0x40), -32.0f);
    EXPECT_FLOAT_EQ(OutputVolumeToDb(0x7F), -0.5f);
    // Out-of-range raw values clamp rather than reading as a positive gain.
    EXPECT_FLOAT_EQ(OutputVolumeToDb(0xFF), 0.0f);
}

TEST(MotuRegisterDspTests, OutputVolumeRoundTripsEveryStep) {
    for (uint32_t raw = 0; raw <= kOutputVolumeMaxRaw; ++raw) {
        EXPECT_EQ(OutputVolumeFromDb(OutputVolumeToDb(static_cast<uint8_t>(raw))), raw) << raw;
    }
}

TEST(MotuRegisterDspTests, OutputVolumeFromDbRoundsAndClamps) {
    EXPECT_EQ(OutputVolumeFromDb(-12.0f), 0x68);
    EXPECT_EQ(OutputVolumeFromDb(-12.2f), 0x68);  // nearest half-dB step
    EXPECT_EQ(OutputVolumeFromDb(-12.3f), 0x67);
    EXPECT_EQ(OutputVolumeFromDb(6.0f), kOutputVolumeMaxRaw);
    EXPECT_EQ(OutputVolumeFromDb(-100.0f), 0);
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
