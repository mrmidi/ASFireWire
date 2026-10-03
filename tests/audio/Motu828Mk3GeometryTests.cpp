#include <gtest/gtest.h>

#include "Audio/Protocols/MOTU/MOTU828Mk3Geometry.hpp"
#include "Audio/DriverKit/Config/MOTU/MOTU828Mk3Profile.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"

#include <array>
#include <cstdint>

namespace {

using namespace ASFW::Audio::MOTU;

struct CapturedBaseGeometry {
    uint32_t rate;
    uint8_t rateIndex;
    uint32_t frames;
    uint32_t hostToDevicePcm;
    uint32_t deviceToHostPcm;
    uint32_t hostToDeviceDbs;
    uint32_t deviceToHostDbs;
    uint32_t hostToDevicePacketBytes;
    uint32_t deviceToHostPacketBytes;
};

// Distilled from twelve direction/rate captures made with the original macOS
// driver. The raw captures are not required to run the unit test suite.
constexpr std::array<CapturedBaseGeometry, 6> kCapturedGeometry{{
    {44100,  0,  8, 14, 18, 13, 16,  424,  520},
    {48000,  1,  8, 14, 18, 13, 16,  424,  520},
    {88200,  2, 16, 14, 18, 13, 16,  840, 1032},
    {96000,  3, 16, 14, 18, 13, 16,  840, 1032},
    {176400, 4, 32, 10, 14, 10, 13, 1288, 1672},
    {192000, 5, 32, 10, 14, 10, 13, 1288, 1672},
}};

} // namespace

// Were both formats the same value, V3 would take every branch written for V2.
// Distinct values are what keeps the two paths apart.
static_assert(ASFW::Encoding::AudioWireFormat::kMotuV3Packed !=
                  ASFW::Encoding::AudioWireFormat::kMotuV2,
              "kMotuV3Packed must not alias kMotuV2");

TEST(Motu828Mk3GeometryTests, CanonicalIdentityMatchesCapturedConfigRom) {
    using namespace ASFW::DeviceProfiles::Audio;
    EXPECT_EQ(kMotuVendorId, 0x0001f2u);
    EXPECT_EQ(kMotu828mk3SwVersion, 0x000015u);
    EXPECT_STREQ(kMotuVendorName, "MOTU");
    EXPECT_STREQ(kMotu828Mk3ModelName, "828 Mk3 FireWire");
}

TEST(Motu828Mk3GeometryTests, BaseGeometryMatchesCapturedWireShapes) {
    for (const auto& captured : kCapturedGeometry) {
        SCOPED_TRACE(captured.rate);
        const auto geometry = Build828Mk3Geometry(captured.rate);
        ASSERT_TRUE(geometry.has_value());
        EXPECT_EQ(geometry->rateIndex, captured.rateIndex);
        EXPECT_EQ(geometry->framesPerDataPacket, captured.frames);
        EXPECT_EQ(geometry->hostToDevicePcm, captured.hostToDevicePcm);
        EXPECT_EQ(geometry->deviceToHostPcm, captured.deviceToHostPcm);
        EXPECT_EQ(geometry->hostToDeviceDbs, captured.hostToDeviceDbs);
        EXPECT_EQ(geometry->deviceToHostDbs, captured.deviceToHostDbs);
        EXPECT_EQ(geometry->hostToDevicePacketBytes, captured.hostToDevicePacketBytes);
        EXPECT_EQ(geometry->deviceToHostPacketBytes, captured.deviceToHostPacketBytes);
        EXPECT_EQ(geometry->sphTicksNumerator, 24576000u);
        EXPECT_EQ(geometry->sphTicksDenominator, captured.rate);
        EXPECT_EQ(geometry->presentationLeadTicks, 9216u);
    }
    EXPECT_FALSE(Build828Mk3Geometry(32000).has_value());
    EXPECT_FALSE(Build828Mk3Geometry(384000).has_value());
}

TEST(Motu828Mk3GeometryTests, DecodesAudioBankControlDirectionsAndModes) {
    const auto banks = DecodeOpticalBanks(
        kEnableOpticalInputA |
        kEnableOpticalInputB | kToslinkOpticalInputB |
        kEnableOpticalOutputA | kToslinkOpticalOutputA);
    EXPECT_EQ(banks.inputA, OpticalBankMode::kAdat);
    EXPECT_EQ(banks.inputB, OpticalBankMode::kToslink);
    EXPECT_EQ(banks.outputA, OpticalBankMode::kToslink);
    EXPECT_EQ(banks.outputB, OpticalBankMode::kDisabled);
    EXPECT_TRUE(banks.AnyEnabled());
    EXPECT_FALSE(DecodeOpticalBanks(0).AnyEnabled());
}

TEST(Motu828Mk3GeometryTests, OpticalBanksContributeByDirectionAndRateFamily) {
    const uint32_t banks =
        kEnableOpticalInputA |
        kEnableOpticalInputB | kToslinkOpticalInputB |
        kEnableOpticalOutputA;

    const auto oneX = Build828Mk3Geometry(48000, banks);
    ASSERT_TRUE(oneX.has_value());
    EXPECT_EQ(oneX->deviceToHostPcm, 18u + 8u + 4u);
    EXPECT_EQ(oneX->hostToDevicePcm, 14u + 8u);
    EXPECT_EQ(oneX->deviceToHostDbs, MotuV3Dbs(30));
    EXPECT_EQ(oneX->hostToDeviceDbs, MotuV3Dbs(22));

    const auto twoX = Build828Mk3Geometry(96000, banks);
    ASSERT_TRUE(twoX.has_value());
    EXPECT_EQ(twoX->deviceToHostPcm, 18u + 4u + 4u);
    EXPECT_EQ(twoX->hostToDevicePcm, 14u + 4u);

    const auto fourX = Build828Mk3Geometry(192000, banks);
    ASSERT_TRUE(fourX.has_value());
    EXPECT_EQ(fourX->deviceToHostPcm, 14u);
    EXPECT_EQ(fourX->hostToDevicePcm, 10u);
}

TEST(Motu828Mk3GeometryTests, DbsFormulaIncludesSphAndTwoMessageChunks) {
    EXPECT_EQ(MotuV3Dbs(14), 13u);
    EXPECT_EQ(MotuV3Dbs(18), 16u);
    EXPECT_EQ(MotuV3Dbs(10), 10u);
    EXPECT_EQ(MotuV3Dbs(22), 19u);
    EXPECT_EQ(MotuV3Dbs(30), 25u);
}

TEST(Motu828Mk3GeometryTests, SphStepperDithersFractionalSamplePeriodsWithoutDrift) {
    constexpr std::array<uint32_t, 6> rates{44100, 48000, 88200, 96000, 176400, 192000};
    constexpr uint32_t kSteps = 10000;
    for (const uint32_t rate : rates) {
        SCOPED_TRACE(rate);
        SphTickStepper stepper(rate);
        uint64_t previous = stepper.Current();
        const uint32_t floorStep = 24576000u / rate;
        for (uint32_t i = 0; i < kSteps; ++i) {
            const uint64_t current = stepper.Advance();
            const uint64_t delta = current - previous;
            EXPECT_TRUE(delta == floorStep || delta == floorStep + 1u);
            previous = current;
        }
        EXPECT_EQ(stepper.Current(),
                  (static_cast<uint64_t>(kSteps) * 24576000u) / rate);
    }
}

TEST(Motu828Mk3GeometryTests, AdkProfilePublishesOnlyValidated48kV3Geometry) {
    ASFW::Isoch::Audio::MOTU::Profiles::MOTU828Mk3Profile profile;
    ASFW::Isoch::Audio::AudioStreamConfig tx{};
    ASFW::Isoch::Audio::AudioStreamConfig rx{};

    ASSERT_TRUE(profile.BuildDefaultTxStreamConfig(tx));
    ASSERT_TRUE(profile.BuildDefaultRxStreamConfig(rx));
    EXPECT_EQ(tx.sampleRate, 48000U);
    EXPECT_EQ(tx.pcmChannels, 14U);
    EXPECT_EQ(tx.dbs, 13U);
    EXPECT_EQ(tx.framesPerDataPacket, 8U);
    EXPECT_EQ(tx.fdf, 0x22U);
    EXPECT_EQ(tx.fmt, 0x02U);
    EXPECT_EQ(rx.pcmChannels, 18U);
    EXPECT_EQ(rx.dbs, 16U);
    EXPECT_EQ(profile.TxWireFormat(), ASFW::Encoding::AudioWireFormat::kMotuV3Packed);
    EXPECT_EQ(profile.RxWireFormat(), ASFW::Encoding::AudioWireFormat::kMotuV3Packed);
    EXPECT_EQ(profile.SupportedSampleRates(), std::vector<uint32_t>({48000U}));

    const auto policy = profile.TxStreamPolicy();
    EXPECT_EQ(policy.hostToDevicePcmEncoding,
              ASFW::Encoding::AudioWireFormat::kMotuV3Packed);
    EXPECT_FALSE(policy.initializeNonAudioSlots);
    EXPECT_TRUE(policy.preserveFdfInNoDataPackets);
}

