#include "Audio/DriverKit/Runtime/HostOutputGain.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using ASFW::Audio::Runtime::HostOutputGain;
using ASFW::Audio::Runtime::HostOutputGainFromDecibels;

constexpr uint32_t kChannels = 2;
constexpr uint32_t kCapacity = 1024;

std::vector<float> OnesRing() {
    return std::vector<float>(static_cast<size_t>(kCapacity) * kChannels, 1.0f);
}

TEST(HostOutputGainTests, DecibelCurveIsAmplitudeWithSilentFloor) {
    EXPECT_FLOAT_EQ(HostOutputGainFromDecibels(0.0f, -80.0f), 1.0f);
    EXPECT_FLOAT_EQ(HostOutputGainFromDecibels(3.0f, -80.0f), 1.0f);
    EXPECT_NEAR(HostOutputGainFromDecibels(-6.0206f, -80.0f), 0.5f, 1e-5f);
    EXPECT_NEAR(HostOutputGainFromDecibels(-40.0f, -80.0f), 0.01f, 1e-6f);
    EXPECT_EQ(HostOutputGainFromDecibels(-80.0f, -80.0f), 0.0f);
    EXPECT_EQ(HostOutputGainFromDecibels(-120.0f, -80.0f), 0.0f);
    EXPECT_EQ(HostOutputGainFromDecibels(std::numeric_limits<float>::quiet_NaN(), -80.0f), 0.0f);
}

// At unity the stage must leave CoreAudio's samples bit-for-bit alone: a device that never
// touches the volume plays exactly what it played before the stage existed.
TEST(HostOutputGainTests, UnityLeavesTheRingUntouched) {
    HostOutputGain gain;
    std::vector<float> ring(static_cast<size_t>(kCapacity) * kChannels);
    for (size_t i = 0; i < ring.size(); ++i) {
        ring[i] = std::nextafter(static_cast<float>(i) * 1e-4f, 1.0f);
    }
    const auto before = ring;
    gain.Apply(ring.data(), kChannels, kCapacity, 0, kCapacity);
    EXPECT_EQ(ring, before);
}

TEST(HostOutputGainTests, ChangeRampsOverRampFramesThenHolds) {
    HostOutputGain gain;
    auto ring = OnesRing();
    gain.SetTarget(0.5f);
    gain.Apply(ring.data(), kChannels, kCapacity, 0, kCapacity);

    // Monotonic descent, both channels of a frame alike, no step larger than one ramp step.
    const float step = 0.5f / static_cast<float>(HostOutputGain::kRampFrames);
    float previous = 1.0f;
    for (uint32_t frame = 0; frame < HostOutputGain::kRampFrames; ++frame) {
        const float left = ring[frame * kChannels];
        EXPECT_EQ(left, ring[frame * kChannels + 1]) << frame;
        EXPECT_LE(left, previous) << frame;
        EXPECT_NEAR(previous - left, step, 1e-5f) << frame;
        previous = left;
    }
    EXPECT_EQ(ring[(HostOutputGain::kRampFrames - 1) * kChannels], 0.5f);
    for (uint32_t frame = HostOutputGain::kRampFrames; frame < kCapacity; ++frame) {
        EXPECT_EQ(ring[frame * kChannels], 0.5f) << frame;
    }
    EXPECT_EQ(gain.Current(), 0.5f);
}

// CoreAudio writes in IO-sized spans; a ramp started in one WriteEnd continues in the next.
TEST(HostOutputGainTests, RampContinuesAcrossWriteEnds) {
    HostOutputGain split;
    HostOutputGain whole;
    auto splitRing = OnesRing();
    auto wholeRing = OnesRing();
    split.SetTarget(0.25f);
    whole.SetTarget(0.25f);
    for (uint32_t first = 0; first < 768; first += 128) {
        split.Apply(splitRing.data(), kChannels, kCapacity, first, 128);
    }
    whole.Apply(wholeRing.data(), kChannels, kCapacity, 0, 768);
    EXPECT_EQ(splitRing, wholeRing);
}

TEST(HostOutputGainTests, SpanWrapsAroundTheRing) {
    HostOutputGain gain;
    auto ring = OnesRing();
    gain.SetTarget(0.0f);
    // Settle the ramp, then scale a span that crosses the ring end.
    std::vector<float> scratch = OnesRing();
    gain.Apply(scratch.data(), kChannels, kCapacity, 0, HostOutputGain::kRampFrames);
    gain.Apply(ring.data(), kChannels, kCapacity, kCapacity * 5 + kCapacity - 4, 8);
    for (uint32_t frame = 0; frame < kCapacity; ++frame) {
        const bool inSpan = frame >= kCapacity - 4 || frame < 4;
        EXPECT_EQ(ring[frame * kChannels], inSpan ? 0.0f : 1.0f) << frame;
    }
}

// A new target mid-ramp starts a fresh ramp from where the gain is, not from the old target:
// a held volume key retargets every few milliseconds and must not jump.
TEST(HostOutputGainTests, RetargetMidRampStartsFromCurrentGain) {
    HostOutputGain gain;
    auto ring = OnesRing();
    gain.SetTarget(0.0f);
    gain.Apply(ring.data(), kChannels, kCapacity, 0, HostOutputGain::kRampFrames / 2);
    const float midway = gain.Current();
    EXPECT_NEAR(midway, 0.5f, 1e-3f);

    gain.SetTarget(1.0f);
    gain.Apply(ring.data(), kChannels, kCapacity, HostOutputGain::kRampFrames / 2, 1);
    EXPECT_GT(gain.Current(), midway);
    EXPECT_LT(gain.Current() - midway, 2.0f / static_cast<float>(HostOutputGain::kRampFrames));
}

// The mute key: silence through the ramp (no click), and unmute returns to the volume the
// user had, not to unity.
TEST(HostOutputGainTests, MuteRampsToSilenceAndUnmuteRestoresTheVolume) {
    HostOutputGain gain;
    auto ring = OnesRing();
    gain.SetTarget(0.5f);
    gain.Apply(ring.data(), kChannels, kCapacity, 0, HostOutputGain::kRampFrames);
    ASSERT_EQ(gain.Current(), 0.5f);

    gain.SetMuted(true);
    EXPECT_TRUE(gain.Muted());
    EXPECT_EQ(gain.Target(), 0.5f);
    ring = OnesRing();
    gain.Apply(ring.data(), kChannels, kCapacity, 0, kCapacity);
    const float step = 0.5f / static_cast<float>(HostOutputGain::kRampFrames);
    float previous = 0.5f;
    for (uint32_t frame = 0; frame < HostOutputGain::kRampFrames; ++frame) {
        EXPECT_NEAR(previous - ring[frame * kChannels], step, 1e-5f) << frame;
        previous = ring[frame * kChannels];
    }
    for (uint32_t frame = HostOutputGain::kRampFrames; frame < kCapacity; ++frame) {
        EXPECT_EQ(ring[frame * kChannels], 0.0f) << frame;
    }

    gain.SetMuted(false);
    ring = OnesRing();
    gain.Apply(ring.data(), kChannels, kCapacity, 0, kCapacity);
    EXPECT_GT(ring[0], 0.0f);
    EXPECT_LT(ring[0], 2.0f * step);
    EXPECT_EQ(ring[(kCapacity - 1) * kChannels], 0.5f);
    EXPECT_EQ(gain.Current(), 0.5f);
}

// A volume key pressed while muted moves the remembered volume but stays silent.
TEST(HostOutputGainTests, VolumeChangeWhileMutedStaysSilent) {
    HostOutputGain gain;
    gain.SetMuted(true);
    auto ring = OnesRing();
    gain.Apply(ring.data(), kChannels, kCapacity, 0, kCapacity);
    gain.SetTarget(0.25f);
    ring = OnesRing();
    gain.Apply(ring.data(), kChannels, kCapacity, 0, kCapacity);
    for (uint32_t frame = 0; frame < kCapacity; ++frame) {
        EXPECT_EQ(ring[frame * kChannels], 0.0f) << frame;
    }
    gain.SetMuted(false);
    ring = OnesRing();
    gain.Apply(ring.data(), kChannels, kCapacity, 0, kCapacity);
    EXPECT_EQ(gain.Current(), 0.25f);
}

TEST(HostOutputGainTests, InvalidTargetIsSilence) {
    HostOutputGain gain;
    gain.SetTarget(std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(gain.Target(), 0.0f);
    gain.SetTarget(-1.0f);
    EXPECT_EQ(gain.Target(), 0.0f);
}

} // namespace
