#include "Audio/Model/AvcRateConfiguration.hpp"
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

TEST(ResolvedAudioConfigurationTests, HardwareBatchExemptionIsExplicitAndDoesNotValidateCapability) {
    auto formation = Formation(96000);
    formation.hardwareValidated = false;
    ASSERT_TRUE(ResolveAudioConfiguration(96000, {&formation, 1}, policy, allocation, 1,
        ConfigurationValidationPolicy::HardwareBatch));
    EXPECT_FALSE(formation.hardwareValidated);
    EXPECT_EQ(ResolveAudioConfiguration(96000, {&formation, 1}, policy, allocation, 1).error(),
        ConfigurationError::HardwareUnvalidated);
}

TEST(ResolvedAudioConfigurationTests, MaximumCapacityIncludesWidestDirectionAndFourTimesRing) {
    std::array formations{Formation(48000, 16), Formation(96000, 12), Formation(192000, 8)};
    formations[0].playback = {{24, 25, 1, {}}};
    const auto maximum = MaximumFormationAllocation(formations, {24576, 2, 2, 128});
    ASSERT_TRUE(maximum);
    EXPECT_EQ(maximum->frameCapacity, 49152U);
    EXPECT_EQ(maximum->playbackChannelCapacity, 24U);
    EXPECT_EQ(maximum->captureChannelCapacity, 16U);
    for (const auto& formation : formations)
        EXPECT_TRUE(ResolveAudioConfiguration(formation.sampleRateHz, formations, policy, *maximum, 1));
    formations[1].capture[0].dataBlockSize = 1;
    EXPECT_FALSE(MaximumFormationAllocation(formations, {}));
}

TEST(ResolvedAudioConfigurationTests, ModelProjectionUsesIndependentDuplexShapesAndRetainsPreferences) {
    ASFW::Audio::Model::ASFWAudioDevice prior;
    prior.currentSampleRate = 48000; prior.outputChannelCount = 16; prior.inputChannelCount = 16;
    auto high = Formation(96000, 8);
    high.capture = {{12, 13, 1, {}}};
    prior.rateFormationCandidates = {Formation(48000, 16), high};
    prior.outputChannelNames = {"old ADAT label"};
    prior.captureStreams = {{.pcmChannels = 16, .am824Slots = 17}};
    prior.playbackStreams = prior.captureStreams;
    const auto next = ASFW::Audio::Model::WithAvcRateFormation(prior, 96000);
    ASSERT_TRUE(next);
    EXPECT_EQ(next->outputChannelCount, 8U);
    EXPECT_EQ(next->inputChannelCount, 12U);
    EXPECT_EQ(next->captureStreams[0].am824Slots, 13U);
    EXPECT_TRUE(next->outputChannelNames.empty());
    EXPECT_EQ(prior.currentSampleRate, 48000U);
    EXPECT_FALSE(ASFW::Audio::Model::WithAvcRateFormation(prior, 88200));
}

TEST(ResolvedAudioConfigurationTests, StreamCountCannotOverflowFixedProductionArrays) {
    auto formation = Formation(48000, 2);
    // assign can reallocate: its fill value must not alias the old storage.
    const auto stream = formation.playback.front();
    formation.playback.assign(5, stream);
    const auto result = ResolveAudioConfiguration(48000, {&formation, 1}, policy, allocation, 0);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), ConfigurationError::InvalidFormation);
}
