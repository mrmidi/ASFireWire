#include "Audio/DriverKit/Runtime/HostOutputVolumePolicy.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace {

using ASFW::Audio::Runtime::CoreAudioVolumeSetting;
using ASFW::Audio::Runtime::HostOutputGain;
using ASFW::Audio::Runtime::ReleaseHostOutputGain;

constexpr uint32_t kChannels = 2;
constexpr uint32_t kCapacity = 1024;

TEST(HostOutputVolumePolicyTests, SharedSettingDefaultsToEnabledAndHoldsTheChoice) {
    auto& setting = CoreAudioVolumeSetting::Shared();
    EXPECT_TRUE(setting.Enabled());
    setting.SetEnabled(false);
    EXPECT_FALSE(setting.Enabled());
    setting.SetEnabled(true);
    EXPECT_TRUE(setting.Enabled());
}

// Withdrawing the controls returns the output to the device's own level (unity, unmuted)
// through the ramp, then leaves CoreAudio's samples alone.
TEST(HostOutputVolumePolicyTests, ReleaseReturnsAttenuatedMutedOutputToUnity) {
    HostOutputGain gain;
    gain.SetTarget(0.25f);
    gain.SetMuted(true);
    std::vector<float> ring(static_cast<size_t>(kCapacity) * kChannels, 1.0f);
    gain.Apply(ring.data(), kChannels, kCapacity, 0, HostOutputGain::kRampFrames);
    ASSERT_EQ(gain.Current(), 0.0f);

    ReleaseHostOutputGain(gain);
    EXPECT_FALSE(gain.Muted());
    EXPECT_EQ(gain.Target(), 1.0f);

    std::fill(ring.begin(), ring.end(), 1.0f);
    gain.Apply(ring.data(), kChannels, kCapacity, HostOutputGain::kRampFrames,
               HostOutputGain::kRampFrames);
    EXPECT_EQ(gain.Current(), 1.0f);
    // The ramp rises: the first released frame is still quiet, the last is at unity.
    const size_t first = static_cast<size_t>(HostOutputGain::kRampFrames % kCapacity) * kChannels;
    EXPECT_LT(ring[first], 0.01f);
    const size_t last =
        static_cast<size_t>((2 * HostOutputGain::kRampFrames - 1) % kCapacity) * kChannels;
    EXPECT_EQ(ring[last], 1.0f);

    std::fill(ring.begin(), ring.end(), 0.5f);
    gain.Apply(ring.data(), kChannels, kCapacity, 2 * HostOutputGain::kRampFrames, 64);
    for (float sample : ring) {
        ASSERT_EQ(sample, 0.5f);
    }
}

} // namespace
