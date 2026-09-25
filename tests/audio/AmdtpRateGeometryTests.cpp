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
    const auto profile = ASFW::IsochTransport::HalBufferProfileForRate(48000);
    EXPECT_EQ(Geometry::kAllocatedFrameRingFrames, 24576U);
    EXPECT_EQ(Geometry::kHalIoPeriodFrames, profile.clientIoBudgetFrames);
    EXPECT_EQ(Geometry::kMaxClientIoFrames, 4096U);
    EXPECT_EQ(Geometry::kFrameAlignment, 32U);
    EXPECT_EQ(Geometry::kRxPacketsPerGroup, 8U);
    EXPECT_EQ(Geometry::kTxPacketsPerGroup, 8U);
    EXPECT_EQ(Geometry::kNominalFramesPerTimingGroup, 48U);
    EXPECT_EQ(Geometry::kRxDescriptorPackets, 504U);
    // The content horizon is the Apple-comparable 400 cycles, floored at the
    // largest client write AudioDriverKit permits (4096) plus scheduling
    // jitter: a 4096-frame WriteEnd must land on exposed packets (Defect B,
    // tools/tx_data_horizon_burst_sim.py --io-frames 4096).
    EXPECT_EQ(Geometry::kTxDataHorizonPackets, 400U);
    EXPECT_EQ(Geometry::TxDataHorizonFrames(48000), 4160U);
    EXPECT_EQ(Geometry::TxDataHorizonFrames(44100), 4160U);
    EXPECT_EQ(Geometry::TxDataHorizonFrames(96000), 4800U);
    EXPECT_EQ(Geometry::kTxSharedSlotPackets, 1696U);
    EXPECT_EQ(Geometry::kTimelineSlots, Geometry::kTxSharedSlotPackets);
    EXPECT_EQ(Geometry::kTxHardwareRingPackets, 48U);
    EXPECT_EQ(Geometry::kTxPreparationLatencyHistogramBuckets, 6U);
    EXPECT_EQ(Geometry::kTxCommittedMarginHistogramBuckets, 5U);
    EXPECT_EQ(Geometry::kTxPreparationLatency250Us, 250U);
    EXPECT_EQ(Geometry::kTxPreparationLatency1500Us, 1500U);
    EXPECT_EQ(Geometry::kTxCommittedMargin2xFloorPackets, 96U);
    EXPECT_EQ(Geometry::kTxCommittedMargin16xFloorPackets, 768U);
    EXPECT_EQ(Geometry::kTxPreparationSlackPackets, 96U);
    EXPECT_EQ(Geometry::kTxCoverageLeadPackets, 144U);
    EXPECT_EQ(Geometry::kTxExposureLeadFrames, 4160U);
    EXPECT_EQ(Geometry::kTxExposureLeadPackets, 760U);
    EXPECT_EQ(Geometry::kTxFrameExposureWindowPackets, 1504U);
    EXPECT_EQ(Geometry::kTxPreparationLeadPackets, 1648U);

    // DMA completion cadence and the ZTS grid are intentionally independent,
    // but the V3 period is a whole number of completion groups (256).
    EXPECT_EQ(profile.zeroTimestampPeriodFrames %
                  Geometry::kNominalFramesPerTimingGroup, 0U);
    EXPECT_EQ(profile.zeroTimestampPeriodFrames /
                  Geometry::kNominalFramesPerTimingGroup, 256U);
    EXPECT_EQ(Geometry::kAllocatedFrameRingFrames %
                  profile.zeroTimestampPeriodFrames, 0U);
    EXPECT_EQ(Geometry::kRxDescriptorPackets %
                  Geometry::kTimingGroupPackets, 0U);
    EXPECT_EQ(Geometry::kRxDescriptorPackets %
                  Geometry::kCadenceBlockPackets, 0U);
    EXPECT_GE(Geometry::kTxPreparationSlackPackets,
              2U * Geometry::kTxPacketsPerGroup);
}

TEST(AudioTimingGeometryTests, V3HalBufferProfilePerRateTier) {
    using namespace ASFW::IsochTransport;

    struct Expected {
        uint32_t rate;
        uint32_t ring;
        bool fits;
    };
    constexpr std::array expected{
        Expected{32000, 12288, true},  Expected{44100, 12288, true},
        Expected{48000, 12288, true},  Expected{88200, 24576, true},
        Expected{96000, 24576, true},  Expected{176400, 49152, false},
        Expected{192000, 49152, false},
    };
    for (const auto& value : expected) {
        const auto profile = HalBufferProfileForRate(value.rate);
        EXPECT_TRUE(IsValidAudioHalBufferProfile(profile)) << value.rate;
        EXPECT_EQ(profile.frameRingFrames, value.ring) << value.rate;
        EXPECT_EQ(profile.zeroTimestampPeriodFrames, value.ring) << value.rate;
        EXPECT_EQ(profile.clientIoBudgetFrames, 1024U) << value.rate;
        EXPECT_EQ(ProfileFitsAllocation(profile), value.fits) << value.rate;
        EXPECT_EQ(AdkMaxClientIoFrames(profile.zeroTimestampPeriodFrames), 4096U)
            << value.rate;
    }
    EXPECT_FALSE(IsValidAudioHalBufferProfile(HalBufferProfileForRate(22050)));
    EXPECT_EQ(HalRateTier(22050), 0U);
    EXPECT_EQ(kAllocatedFrameRingFrames, 24576U);
    // A smaller period caps AudioDriverKit clients below 4096 frames.
    EXPECT_EQ(AdkMaxClientIoFrames(8192), 3072U);
}

} // namespace
