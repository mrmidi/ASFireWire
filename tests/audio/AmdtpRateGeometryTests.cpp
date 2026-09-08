#include "Audio/Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "Audio/Shared/AudioTimingGeometry.hpp"
#include "Isoch/Core/IsochDmaGeometry.hpp"

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

TEST(AudioTimingGeometryTests, V3GeometryIsUnified) {
    using Geometry =
        ASFW::Audio::Shared::AudioTimingGeometry;
    const auto profile =
        ASFW::Audio::Shared::kActiveAudioHalBufferProfile;
    EXPECT_EQ(Geometry::kFrameRingFrames, profile.frameRingFrames);
    EXPECT_EQ(Geometry::kHalIoPeriodFrames, profile.clientIoBudgetFrames);
    EXPECT_EQ(
        Geometry::kHalZeroTimestampPeriodFrames,
        profile.zeroTimestampPeriodFrames);
    EXPECT_EQ(Geometry::kFrameAlignment, 32U);
    // Eight packets is two whole D,D,D,N cadence blocks, so the interrupt
    // yield is phase-independent: min == max == 48 frames at 1x. It was 6
    // packets and 32/40/36 until 2026-09-08.
    EXPECT_EQ(Geometry::kRxPacketsPerGroup, 8U);
    EXPECT_EQ(Geometry::kTxPacketsPerGroup, 8U);
    EXPECT_EQ(Geometry::kMinimumNominalFramesPerInterrupt, 48U);
    EXPECT_EQ(Geometry::kMaximumNominalFramesPerInterrupt, 48U);
    EXPECT_EQ(Geometry::kNominalFramesPerTimingGroup, 48U);
    EXPECT_EQ(ASFW::Isoch::IsochDmaGeometry::kReceiveDescriptorPackets, 504U);
    EXPECT_EQ(Geometry::kPcmPublicationCacheFrames, 12288U);
    EXPECT_EQ(Geometry::kTxSharedSlotPackets, 1512U);
    EXPECT_EQ(Geometry::kTxHardwareRingPackets, 504U);
    EXPECT_EQ(Geometry::kTxPreparationLatencyHistogramBuckets, 6U);
    EXPECT_EQ(Geometry::kTxCommittedMarginHistogramBuckets, 5U);
    EXPECT_EQ(Geometry::kTxPreparationLatency250Us, 250U);
    EXPECT_EQ(Geometry::kTxPreparationLatency1500Us, 1500U);
    // Committed-margin buckets resolve fractions of the hardware ring: the
    // shared store is three rings deep, so the old 2x/4x/8x/16x-ring ladder put
    // every sample in one bucket.
    EXPECT_EQ(Geometry::kTxCommittedMarginQuarterRingPackets, 126U);
    EXPECT_EQ(Geometry::kTxCommittedMarginHalfRingPackets, 252U);
    EXPECT_EQ(Geometry::kTxCommittedMarginThreeQuarterRingPackets, 378U);
    EXPECT_EQ(Geometry::kTxCommittedMarginOneRingPackets, 504U);
    EXPECT_EQ(Geometry::kTxPreparationSlackPackets, 504U);
    EXPECT_EQ(Geometry::kTxCoverageLeadPackets, 1008U);
    EXPECT_EQ(Geometry::kTxPreparationLeadPackets, 1008U);

    // DMA completion cadence and the ZTS grid are intentionally independent.
    EXPECT_NE(Geometry::kHalZeroTimestampPeriodFrames,
              Geometry::kNominalFramesPerTimingGroup);
    EXPECT_EQ(Geometry::kFrameRingFrames %
                  Geometry::kHalZeroTimestampPeriodFrames, 0U);
    EXPECT_EQ(Geometry::kFrameRingFrames %
                  Geometry::kHalIoPeriodFrames, 0U);
    EXPECT_EQ(ASFW::Isoch::IsochDmaGeometry::kReceiveDescriptorPackets %
                  Geometry::kTimingGroupPackets, 0U);
    EXPECT_EQ(ASFW::Isoch::IsochDmaGeometry::kReceiveDescriptorPackets %
                  Geometry::kCadenceBlockPackets, 0U);
    EXPECT_GE(Geometry::kTxPreparationSlackPackets,
              2U * Geometry::kTxPacketsPerGroup);
}

TEST(AudioTimingGeometryTests, V3PublishesOnlyExactIntegerTickRates) {
    using Geometry = ASFW::Audio::Shared::AudioTimingGeometry;
    EXPECT_TRUE(Geometry::IsV3SampleRate(48'000));
    EXPECT_TRUE(Geometry::IsV3SampleRate(96'000));
    EXPECT_TRUE(Geometry::IsV3SampleRate(192'000));
    EXPECT_FALSE(Geometry::IsV3SampleRate(44'100));
    EXPECT_FALSE(Geometry::IsV3SampleRate(88'200));
    EXPECT_FALSE(Geometry::IsV3SampleRate(176'400));
}

TEST(AudioTimingGeometryTests, HalBufferProfileIsGlobalV3Geometry) {
    using namespace ASFW::Audio::Shared;
    EXPECT_EQ(kAudioHalBufferProfileV3.frameRingFrames, 12288U);
    EXPECT_EQ(kAudioHalBufferProfileV3.clientIoBudgetFrames, 1024U);
    EXPECT_EQ(kAudioHalBufferProfileV3.zeroTimestampPeriodFrames, 12288U);
    EXPECT_TRUE(IsValidAudioHalBufferProfile(kActiveAudioHalBufferProfile));
    EXPECT_EQ(kActiveAudioHalBufferProfile.frameRingFrames, 12288U);
}

} // namespace
