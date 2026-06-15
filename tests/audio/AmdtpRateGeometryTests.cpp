#include "Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "Shared/Isoch/AudioTimingGeometry.hpp"

#include <gtest/gtest.h>

#include <array>

namespace {

TEST(AmdtpRateGeometryTests, StandardRatesKeepNominalAndSytIntervalDistinct) {
    struct Expected {
        uint32_t rate;
        uint32_t nominal;
        uint32_t interval;
    };
    constexpr std::array expected{
        Expected{32000, 4, 8},
        Expected{44100, 6, 8},
        Expected{48000, 6, 8},
        Expected{88200, 12, 16},
        Expected{96000, 12, 16},
        Expected{176400, 24, 32},
        Expected{192000, 24, 32},
    };

    for (const auto& value : expected) {
        const auto geometry = ASFW::Encoding::AmdtpRateGeometryForSampleRate(value.rate);
        ASSERT_TRUE(geometry.has_value());
        EXPECT_EQ(geometry->sampleRateHz, value.rate);
        EXPECT_EQ(geometry->nominalFramesPerCycle, value.nominal);
        EXPECT_EQ(geometry->sytIntervalFrames, value.interval);
    }
    EXPECT_FALSE(ASFW::Encoding::AmdtpRateGeometryForSampleRate(96001).has_value());
}

TEST(AudioTimingGeometryTests, SaffireGeometryIsUnified) {
    using Geometry =
        ASFW::IsochTransport::AudioTimingGeometry;
    const auto profile =
        ASFW::IsochTransport::kActiveAudioHalBufferProfile;
    EXPECT_EQ(Geometry::kFrameRingFrames, profile.frameRingFrames);
    EXPECT_EQ(Geometry::kHalIoPeriodFrames, profile.clientIoBudgetFrames);
    EXPECT_EQ(
        Geometry::kHalZeroTimestampPeriodFrames,
        profile.zeroTimestampPeriodFrames);
    EXPECT_EQ(Geometry::kFrameAlignment, 32U);
    EXPECT_EQ(Geometry::kRxPacketsPerGroup, 6U);
    EXPECT_EQ(Geometry::kTxPacketsPerGroup, 6U);
    EXPECT_EQ(Geometry::kMinimumNominalFramesPerInterrupt, 32U);
    EXPECT_EQ(Geometry::kMaximumNominalFramesPerInterrupt, 40U);
    EXPECT_EQ(Geometry::kNominalFramesPerTimingGroup, 36U);
    EXPECT_EQ(Geometry::kInputSafetyFloorFrames, 104U);
    EXPECT_EQ(Geometry::kRxDescriptorPackets, 504U);
    EXPECT_EQ(Geometry::kTxSharedSlotPackets, 192U);
    EXPECT_EQ(Geometry::kTxHardwareRingPackets, 48U);
    EXPECT_EQ(Geometry::kTxPreparationSlackPackets, 96U);
    EXPECT_EQ(Geometry::kTxPreparationLeadPackets, 144U);

    // DMA completion cadence and the ZTS grid are intentionally independent.
    EXPECT_NE(Geometry::kHalZeroTimestampPeriodFrames,
              Geometry::kNominalFramesPerTimingGroup);
    EXPECT_EQ(Geometry::kFrameRingFrames %
                  Geometry::kHalZeroTimestampPeriodFrames, 0U);
    EXPECT_EQ(Geometry::kFrameRingFrames %
                  Geometry::kHalIoPeriodFrames, 0U);
    EXPECT_EQ(Geometry::kRxDescriptorPackets %
                  Geometry::kTimingGroupPackets, 0U);
    EXPECT_EQ(Geometry::kRxDescriptorPackets %
                  Geometry::kCadenceBlockPackets, 0U);
    EXPECT_GE(Geometry::kTxPreparationSlackPackets,
              2U * Geometry::kTxPacketsPerGroup);
}

TEST(AudioTimingGeometryTests, HalBufferProfilesPreserveKnownGeometries) {
    using namespace ASFW::IsochTransport;

    EXPECT_EQ(kAudioHalBufferProfileAligned512.frameRingFrames, 512U);
    EXPECT_EQ(kAudioHalBufferProfileAligned512.clientIoBudgetFrames, 512U);
    EXPECT_EQ(kAudioHalBufferProfileAligned512.zeroTimestampPeriodFrames, 512U);

    EXPECT_EQ(kAudioHalBufferProfilePreDiceZts192.frameRingFrames, 1536U);
    EXPECT_EQ(
        kAudioHalBufferProfilePreDiceZts192.clientIoBudgetFrames,
        512U);
    EXPECT_EQ(
        kAudioHalBufferProfilePreDiceZts192.zeroTimestampPeriodFrames,
        192U);

    EXPECT_EQ(kAudioHalBufferProfileDiceWorking1536.frameRingFrames, 1536U);
    EXPECT_EQ(
        kAudioHalBufferProfileDiceWorking1536.clientIoBudgetFrames,
        512U);
    EXPECT_EQ(
        kAudioHalBufferProfileDiceWorking1536.zeroTimestampPeriodFrames,
        1536U);

    EXPECT_TRUE(IsValidAudioHalBufferProfile(kActiveAudioHalBufferProfile));
#if !defined(ASFW_AUDIO_HAL_BUFFER_PROFILE)
    EXPECT_EQ(kActiveAudioHalBufferProfileId,
              AudioHalBufferProfileId::DiceWorking1536);
#endif
}

} // namespace
