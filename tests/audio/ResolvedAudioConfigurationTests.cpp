#include "Audio/Runtime/ResolvedAudioConfiguration.hpp"
#include <gtest/gtest.h>

using namespace ASFW::Audio::Runtime;
namespace {
const DeviceTimingPolicy policy{64, 64, 128, 128};
const ConfigurationAllocation allocation{49152, 32, 32, 4104};
RateFormation Formation(uint32_t rate, uint32_t channels = 10) {
    return {.sampleRateHz = rate,
            .playback = {{channels, channels + 1, 1, {}}},
            .capture = {{channels, channels + 1, 1, {}}},
            .protocolSupported = true, .hardwareValidated = true};
}
}

TEST(ResolvedAudioConfigurationTests, AllRatesBothModesResolveOneCoherentGeometry) {
    for (auto mode : {ASFW::Encoding::StreamMode::kBlocking, ASFW::Encoding::StreamMode::kNonBlocking}) {
        for (uint32_t rate : {32000U, 44100U, 48000U, 88200U, 96000U, 176400U, 192000U}) {
            auto formation = Formation(rate);
            formation.mode = mode;
            const auto result = ResolveAudioConfiguration(rate, {&formation, 1}, policy, allocation, 7);
            ASSERT_TRUE(result) << rate;
            EXPECT_EQ(result->timing.sampleRateHz, result->formation.sampleRateHz);
            EXPECT_EQ(result->revision, 7);
            EXPECT_LE(result->timing.frameRingFrames, allocation.frameCapacity);
            EXPECT_EQ(result->playbackAllocationBytes, 49152ULL * 32 * 4);
        }
    }
}

TEST(ResolvedAudioConfigurationTests, UsesRateSpecificAdatChannelsInsteadOfScaling) {
    std::array formations{Formation(48000, 16), Formation(96000, 12)};
    const auto low = ResolveAudioConfiguration(48000, formations, policy, allocation, 1);
    const auto high = ResolveAudioConfiguration(96000, formations, policy, allocation, 2);
    ASSERT_TRUE(low); ASSERT_TRUE(high);
    EXPECT_EQ(low->captureChannels, 16);
    EXPECT_EQ(high->captureChannels, 12);
    EXPECT_EQ(high->formation.capture[0].dataBlockSize, 13);
    EXPECT_EQ(low->timing.zeroTimestampPeriodFrames, 12288);
    EXPECT_EQ(high->timing.zeroTimestampPeriodFrames, 24576);
}

TEST(ResolvedAudioConfigurationTests, CapabilityAndHardwareGatesRejectBeforeSideEffects) {
    auto formation = Formation(48000);
    EXPECT_EQ(ResolveAudioConfiguration(32000, {&formation, 1}, policy, allocation, 1).error(), ConfigurationError::RateNotOffered);
    formation.protocolSupported = false;
    EXPECT_EQ(ResolveAudioConfiguration(48000, {&formation, 1}, policy, allocation, 1).error(), ConfigurationError::ProtocolUnsupported);
    formation.protocolSupported = true;
    formation.hardwareValidated = false;
    EXPECT_EQ(ResolveAudioConfiguration(48000, {&formation, 1}, policy, allocation, 1).error(), ConfigurationError::HardwareUnvalidated);
}

TEST(ResolvedAudioConfigurationTests, RefusesUndersizedMemoryPacketsAndInvalidMaps) {
    auto formation = Formation(192000);
    auto small = allocation;
    small.frameCapacity = 24576;
    EXPECT_FALSE(ResolveAudioConfiguration(192000, {&formation, 1}, policy, small, 1));
    small = allocation;
    small.maxPacketBytes = 100;
    EXPECT_EQ(ResolveAudioConfiguration(192000, {&formation, 1}, policy, small, 1).error(), ConfigurationError::ExceedsAllocation);
    formation.playback[0].pcmSlots.SetSlots(std::array<uint8_t, 10>{});
    EXPECT_EQ(ResolveAudioConfiguration(192000, {&formation, 1}, policy, allocation, 1).error(), ConfigurationError::InvalidFormation);
}

TEST(ResolvedAudioConfigurationTests, AmbiguousOrMissingFormationsAreRejected) {
    std::array formations{Formation(48000), Formation(48000, 12)};
    EXPECT_EQ(ResolveAudioConfiguration(48000, formations, policy, allocation, 1).error(), ConfigurationError::InvalidFormation);
    auto empty = Formation(48000);
    empty.playback.clear(); empty.capture.clear();
    EXPECT_EQ(ResolveAudioConfiguration(48000, {&empty, 1}, policy, allocation, 1).error(), ConfigurationError::InvalidFormation);
}
