// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioGeometryResolverTests.cpp
// Tests for pure DriverKit-independent audio geometry resolver.

#include "Audio/Shared/AudioGeometryResolver.hpp"

#include <gtest/gtest.h>

namespace {

using namespace ASFW::Audio::Shared;

TEST(AudioGeometryResolverTests, Resolves48kHzActiveGeometry) {
    AudioDeviceFormation formation{
        .sampleRateHz = 48'000,
        .inputChannels = 2,
        .outputChannels = 2,
        .fdf = 0x00,
        .sytIntervalFrames = 8,
    };
    AudioDeviceTimingPolicy timingPolicy{
        .txReportedLatencyFrames = 128,
        .rxReportedLatencyFrames = 64,
        .txSafetyOffsetFrames = 48,
        .rxSafetyOffsetFrames = 32,
        .txTransferDelayTicks = 12'800,
        .rxTransferDelayTicks = 12'800,
    };
    DirectAudioAllocationLimits limits{
        .allocatedOutputBytes = 24'576 * 2 * sizeof(float),
        .allocatedInputBytes = 24'576 * 2 * sizeof(float),
        .maxOutputChannels = 2,
        .maxInputChannels = 2,
        .maxAllocatedFrames = 24'576,
    };

    auto result = ResolveAudioGeometry(formation, timingPolicy, nullptr, limits, 42);
    ASSERT_TRUE(result.has_value());

    const auto& geom = result.value();
    EXPECT_EQ(geom.sampleRateHz, 48'000U);
    EXPECT_EQ(geom.topologyRevision, 42U);
    EXPECT_EQ(geom.activeOutputRingFrames, 12'288U);
    EXPECT_EQ(geom.activeInputRingFrames, 12'288U);
    EXPECT_EQ(geom.outputChannels, 2U);
    EXPECT_EQ(geom.inputChannels, 2U);
    EXPECT_EQ(geom.requiredOutputBytes, 12'288U * 2U * sizeof(float));
    EXPECT_EQ(geom.requiredInputBytes, 12'288U * 2U * sizeof(float));
    EXPECT_EQ(geom.zeroTimestampPeriodFrames, 12'288U);
    EXPECT_EQ(geom.clientIoBudgetFrames, 1'024U);
    EXPECT_EQ(geom.pcmCacheCapacityFrames, 12'288U);
    EXPECT_EQ(geom.outputLatencyFrames, 128U);
    EXPECT_EQ(geom.inputLatencyFrames, 64U);
    EXPECT_EQ(geom.outputSafetyOffsetFrames, 60U); // max(60, 48)
    EXPECT_EQ(geom.inputSafetyOffsetFrames, 32U);
    EXPECT_EQ(geom.fdf, 0x00);
    EXPECT_EQ(geom.sytIntervalFrames, 8U);
    EXPECT_EQ(geom.txTransferDelayTicks, 12'800U);
    EXPECT_EQ(geom.rxTransferDelayTicks, 12'800U);
}

TEST(AudioGeometryResolverTests, Resolves96kHzActiveGeometry) {
    AudioDeviceFormation formation{
        .sampleRateHz = 96'000,
        .inputChannels = 4,
        .outputChannels = 4,
        .fdf = 0x01,
        .sytIntervalFrames = 16,
    };
    AudioDeviceTimingPolicy timingPolicy{
        .txReportedLatencyFrames = 256,
        .rxReportedLatencyFrames = 128,
        .txSafetyOffsetFrames = 96,
        .rxSafetyOffsetFrames = 64,
        .txTransferDelayTicks = 12'800,
        .rxTransferDelayTicks = 12'800,
    };
    DirectAudioAllocationLimits limits{
        .allocatedOutputBytes = 24'576 * 4 * sizeof(float),
        .allocatedInputBytes = 24'576 * 4 * sizeof(float),
        .maxOutputChannels = 4,
        .maxInputChannels = 4,
        .maxAllocatedFrames = 24'576,
    };

    auto result = ResolveAudioGeometry(formation, timingPolicy, nullptr, limits, 101);
    ASSERT_TRUE(result.has_value());

    const auto& geom = result.value();
    EXPECT_EQ(geom.sampleRateHz, 96'000U);
    EXPECT_EQ(geom.topologyRevision, 101U);
    EXPECT_EQ(geom.activeOutputRingFrames, 24'576U);
    EXPECT_EQ(geom.activeInputRingFrames, 24'576U);
    EXPECT_EQ(geom.outputChannels, 4U);
    EXPECT_EQ(geom.inputChannels, 4U);
    EXPECT_EQ(geom.requiredOutputBytes, 24'576U * 4U * sizeof(float));
    EXPECT_EQ(geom.requiredInputBytes, 24'576U * 4U * sizeof(float));
    EXPECT_EQ(geom.zeroTimestampPeriodFrames, 24'576U);
    EXPECT_EQ(geom.clientIoBudgetFrames, 1'024U);
    EXPECT_EQ(geom.pcmCacheCapacityFrames, 24'576U);
    EXPECT_EQ(geom.outputLatencyFrames, 256U);
    EXPECT_EQ(geom.inputLatencyFrames, 128U);
    EXPECT_EQ(geom.outputSafetyOffsetFrames, 120U); // max(120, 96)
    EXPECT_EQ(geom.inputSafetyOffsetFrames, 64U);
    EXPECT_EQ(geom.fdf, 0x01);
    EXPECT_EQ(geom.sytIntervalFrames, 16U);
    EXPECT_EQ(geom.txTransferDelayTicks, 12'800U);
    EXPECT_EQ(geom.rxTransferDelayTicks, 12'800U);
}

TEST(AudioGeometryResolverTests, EnforcesAllocationLimits) {
    AudioDeviceFormation formation{
        .sampleRateHz = 96'000,
        .inputChannels = 2,
        .outputChannels = 2,
    };
    AudioDeviceTimingPolicy timingPolicy{
        .txSafetyOffsetFrames = 48,
    };

    // 1. Output bytes exceeded
    {
        DirectAudioAllocationLimits limits{
            .allocatedOutputBytes = 12'288 * 2 * sizeof(float), // Only enough for 48k
            .allocatedInputBytes = 24'576 * 2 * sizeof(float),
            .maxAllocatedFrames = 24'576,
        };
        auto res = ResolveAudioGeometry(formation, timingPolicy, nullptr, limits, 1);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error(), GeometryError::OutputBytesExceedAllocation);
    }

    // 2. Input bytes exceeded
    {
        DirectAudioAllocationLimits limits{
            .allocatedOutputBytes = 24'576 * 2 * sizeof(float),
            .allocatedInputBytes = 12'288 * 2 * sizeof(float), // Only enough for 48k
            .maxAllocatedFrames = 24'576,
        };
        auto res = ResolveAudioGeometry(formation, timingPolicy, nullptr, limits, 1);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error(), GeometryError::InputBytesExceedAllocation);
    }

    // 3. Max allocated frames exceeded
    {
        DirectAudioAllocationLimits limits{
            .allocatedOutputBytes = 24'576 * 2 * sizeof(float),
            .allocatedInputBytes = 24'576 * 2 * sizeof(float),
            .maxAllocatedFrames = 12'288,
        };
        auto res = ResolveAudioGeometry(formation, timingPolicy, nullptr, limits, 1);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error(), GeometryError::FramesExceedAllocation);
    }
}

TEST(AudioGeometryResolverTests, RejectsUnsupportedSampleRates) {
    for (uint32_t badRate : {0U, 88'200U, 176'400U, 384'000U}) {
        AudioDeviceFormation formation{
            .sampleRateHz = badRate,
            .inputChannels = 2,
            .outputChannels = 2,
        };
        AudioDeviceTimingPolicy timingPolicy{
            .txSafetyOffsetFrames = 48,
        };
        DirectAudioAllocationLimits limits{};
        auto res = ResolveAudioGeometry(formation, timingPolicy, nullptr, limits, 1);
        ASSERT_FALSE(res.has_value());
        EXPECT_EQ(res.error(), GeometryError::UnsupportedSampleRate);
    }
}

TEST(AudioGeometryResolverTests, RejectsZeroTotalChannels) {
    AudioDeviceFormation formation{
        .sampleRateHz = 48'000,
        .inputChannels = 0,
        .outputChannels = 0,
    };
    AudioDeviceTimingPolicy timingPolicy{
        .txSafetyOffsetFrames = 48,
    };
    DirectAudioAllocationLimits limits{};
    auto res = ResolveAudioGeometry(formation, timingPolicy, nullptr, limits, 1);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), GeometryError::InvalidChannelCount);
}

TEST(AudioGeometryResolverTests, AcceptsInputOnlyOrOutputOnlyDevices) {
    AudioDeviceTimingPolicy timingPolicy{
        .txSafetyOffsetFrames = 48,
    };
    DirectAudioAllocationLimits limits{};

    // Output only
    {
        AudioDeviceFormation outOnly{
            .sampleRateHz = 48'000,
            .inputChannels = 0,
            .outputChannels = 2,
        };
        auto res = ResolveAudioGeometry(outOnly, timingPolicy, nullptr, limits, 1);
        ASSERT_TRUE(res.has_value());
        EXPECT_EQ(res->outputChannels, 2U);
        EXPECT_EQ(res->inputChannels, 0U);
        EXPECT_GT(res->requiredOutputBytes, 0U);
        EXPECT_EQ(res->requiredInputBytes, 0U);
    }

    // Input only
    {
        AudioDeviceFormation inOnly{
            .sampleRateHz = 48'000,
            .inputChannels = 2,
            .outputChannels = 0,
        };
        auto res = ResolveAudioGeometry(inOnly, timingPolicy, nullptr, limits, 1);
        ASSERT_TRUE(res.has_value());
        EXPECT_EQ(res->outputChannels, 0U);
        EXPECT_EQ(res->inputChannels, 2U);
        EXPECT_EQ(res->requiredOutputBytes, 0U);
        EXPECT_GT(res->requiredInputBytes, 0U);
    }
}

TEST(AudioGeometryResolverTests, AppliesRuntimeTuningOverrides) {
    AudioDeviceFormation formation{
        .sampleRateHz = 48'000,
        .inputChannels = 2,
        .outputChannels = 2,
    };
    AudioDeviceTimingPolicy timingPolicy{
        .txReportedLatencyFrames = 100,
        .rxReportedLatencyFrames = 80,
        .txSafetyOffsetFrames = 48,
        .rxSafetyOffsetFrames = 32,
    };
    DirectAudioAllocationLimits limits{};

    AudioRuntimeTuning tuning{
        .outputLatencyFrames = 300,
        .inputLatencyFrames = 250,
        .outputSafetyOffsetFrames = 150,
        .inputSafetyOffsetFrames = 120,
        .frameRingFrames = 16'384,
    };

    auto res = ResolveAudioGeometry(formation, timingPolicy, &tuning, limits, 7);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->outputLatencyFrames, 300U);
    EXPECT_EQ(res->inputLatencyFrames, 250U);
    EXPECT_EQ(res->outputSafetyOffsetFrames, 150U);
    EXPECT_EQ(res->inputSafetyOffsetFrames, 120U);
    // pcmCacheCapacityFrames takes tuning frameRingFrames override
    EXPECT_EQ(res->pcmCacheCapacityFrames, 16'384U);
    // But active HAL stream ring remains locked to rate geometry
    EXPECT_EQ(res->activeOutputRingFrames, 12'288U);
    EXPECT_EQ(res->activeInputRingFrames, 12'288U);
}

TEST(AudioGeometryResolverTests, Resolves441kHzActiveGeometry) {
    AudioDeviceFormation formation{
        .sampleRateHz = 44'100,
        .inputChannels = 2,
        .outputChannels = 2,
        .fdf = 0x01,
        .sytIntervalFrames = 8,
    };
    AudioDeviceTimingPolicy timingPolicy{
        .txReportedLatencyFrames = 128,
        .rxReportedLatencyFrames = 64,
        .txSafetyOffsetFrames = 48,
        .rxSafetyOffsetFrames = 32,
        .txTransferDelayTicks = 13'162,
        .rxTransferDelayTicks = 13'162,
    };
    DirectAudioAllocationLimits limits{
        .allocatedOutputBytes = 24'576 * 2 * sizeof(float),
        .allocatedInputBytes = 24'576 * 2 * sizeof(float),
        .maxOutputChannels = 2,
        .maxInputChannels = 2,
        .maxAllocatedFrames = 24'576,
    };

    auto result = ResolveAudioGeometry(formation, timingPolicy, nullptr, limits, 99);
    ASSERT_TRUE(result.has_value());

    const auto& geom = result.value();
    EXPECT_EQ(geom.sampleRateHz, 44'100U);
    EXPECT_EQ(geom.topologyRevision, 99U);
    EXPECT_EQ(geom.activeOutputRingFrames, 12'288U);
    EXPECT_EQ(geom.activeInputRingFrames, 12'288U);
    EXPECT_EQ(geom.outputChannels, 2U);
    EXPECT_EQ(geom.inputChannels, 2U);
    EXPECT_EQ(geom.zeroTimestampPeriodFrames, 12'288U);
    EXPECT_EQ(geom.clientIoBudgetFrames, 1'024U);
    EXPECT_EQ(geom.pcmCacheCapacityFrames, 12'288U);
    EXPECT_EQ(geom.txTransferDelayTicks, 13'162U);
    EXPECT_EQ(geom.rxTransferDelayTicks, 13'162U);
}

} // namespace
