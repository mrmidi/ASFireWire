// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// StreamProfileAggregationTests.cpp
//
// These drive the PRODUCTION accessors -- IAudioStreamProfile::TxChannelCount()
// and RxChannelCount() -- rather than a hand-populated geometry struct. That
// distinction is the point: ResolvedStreamGeometry tests can all stay green
// while the profile aggregates revert to pcmChannels * StreamCount(), because
// they never call them. These fail if that multiplication comes back.
//
// Shapes come from the recorded dumps in documentation/fixtures/DICE/, never
// from a profile constant.

#include "Audio/DriverKit/Config/AudioStreamProfile.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using ASFW::Isoch::Audio::AudioStreamConfig;
using ASFW::Isoch::Audio::IAudioStreamProfile;

// Minimal profile whose per-stream widths are supplied per direction, so a
// single fixture can express both the symmetric and the asymmetric cases.
class TwoStreamProfile : public IAudioStreamProfile {
public:
    TwoStreamProfile(uint16_t playback0, uint16_t playback1,
                     uint16_t capture0, uint16_t capture1) noexcept
        : playback0_(playback0), playback1_(playback1),
          capture0_(capture0), capture1_(capture1) {}

    const char* Name() const noexcept override { return "TwoStreamProfile"; }
    ASFW::Encoding::AudioWireFormat TxWireFormat() const noexcept override { return {}; }
    ASFW::Encoding::AudioWireFormat RxWireFormat() const noexcept override { return {}; }
    uint32_t TxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t RxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t TxReportedLatencyFrames(double) const noexcept override { return 0; }
    uint32_t RxReportedLatencyFrames(double) const noexcept override { return 0; }

    uint32_t TxStreamCount() const noexcept override { return 2; }
    uint32_t RxStreamCount() const noexcept override { return 2; }

    bool BuildDefaultTxStreamConfig(AudioStreamConfig& c) const noexcept override {
        return Fill(c, playback0_, 0);
    }
    bool BuildDefaultRxStreamConfig(AudioStreamConfig& c) const noexcept override {
        return Fill(c, capture0_, 0);
    }
    bool BuildTxStreamConfig(uint32_t i, AudioStreamConfig& c) const noexcept override {
        if (i >= TxStreamCount()) return false;
        return Fill(c, (i == 0) ? playback0_ : playback1_, (i == 0) ? 0 : playback0_);
    }
    bool BuildRxStreamConfig(uint32_t i, AudioStreamConfig& c) const noexcept override {
        if (i >= RxStreamCount()) return false;
        return Fill(c, (i == 0) ? capture0_ : capture1_, (i == 0) ? 0 : capture0_);
    }

private:
    static bool Fill(AudioStreamConfig& c, uint16_t pcm, uint16_t offset) noexcept {
        c = {};
        c.pcmChannels = static_cast<uint8_t>(pcm);
        c.dbs = static_cast<uint8_t>(pcm);
        c.sourceChannelOffset = offset;
        return true;
    }

    uint16_t playback0_{0};
    uint16_t playback1_{0};
    uint16_t capture0_{0};
    uint16_t capture1_{0};
};

// ASFW's profile naming inverts the device's: Tx* is host playback.

// midasF24.txt: playback 16 + 8 = 24. Multiplication would say 32.
TEST(StreamProfileAggregation, VeniceF24PlaybackAggregateSumsStreams) {
    const TwoStreamProfile profile{16, 8, 16, 8};
    EXPECT_EQ(profile.TxChannelCount(), 24u);
    EXPECT_NE(profile.TxChannelCount(), 16u * profile.TxStreamCount());
}

// midasF24.txt: capture 16 + 8 = 24, through the indexed capture accessor.
TEST(StreamProfileAggregation, VeniceF24CaptureAggregateSumsStreams) {
    const TwoStreamProfile profile{16, 8, 16, 8};
    EXPECT_EQ(profile.RxChannelCount(), 24u);
    EXPECT_NE(profile.RxChannelCount(), 16u * profile.RxStreamCount());
}

// presonus2442.txt: capture 16 + 16 = 32, playback 16 + 10 = 26. The two
// directions must be aggregated independently; reusing one for the other is
// wrong by six channels here.
TEST(StreamProfileAggregation, StudioLive2442AggregatesDirectionsIndependently) {
    const TwoStreamProfile profile{16, 10, 16, 16};
    EXPECT_EQ(profile.TxChannelCount(), 26u);
    EXPECT_EQ(profile.RxChannelCount(), 32u);
    EXPECT_NE(profile.TxChannelCount(), profile.RxChannelCount());
}

// A uniform device must keep its old answer: summing 16+16 and multiplying
// 16x2 agree, so this pins that the change is not a behaviour change for the
// devices that were already right.
TEST(StreamProfileAggregation, UniformStreamsAreUnchangedBySumming) {
    const TwoStreamProfile profile{16, 16, 16, 16};
    EXPECT_EQ(profile.TxChannelCount(), 32u);
    EXPECT_EQ(profile.TxChannelCount(), 16u * profile.TxStreamCount());
    EXPECT_EQ(profile.RxChannelCount(), 32u);
}

// The base class default replicates stream 0 at successive offsets. A profile
// that does NOT override the indexed builders still aggregates consistently
// with what it describes, rather than by a separate multiplication.
class UniformDefaultProfile final : public TwoStreamProfile {
public:
    UniformDefaultProfile() noexcept : TwoStreamProfile(12, 12, 12, 12) {}
    // Deliberately no BuildTx/RxStreamConfig override: exercise the base.
    bool BuildTxStreamConfig(uint32_t i, AudioStreamConfig& c) const noexcept override {
        return IAudioStreamProfile::BuildTxStreamConfig(i, c);
    }
    bool BuildRxStreamConfig(uint32_t i, AudioStreamConfig& c) const noexcept override {
        return IAudioStreamProfile::BuildRxStreamConfig(i, c);
    }
};

TEST(StreamProfileAggregation, BaseIndexedBuildersAggregateFromTheDefault) {
    const UniformDefaultProfile profile;
    EXPECT_EQ(profile.TxChannelCount(), 24u);
    EXPECT_EQ(profile.RxChannelCount(), 24u);

    AudioStreamConfig second{};
    ASSERT_TRUE(profile.BuildRxStreamConfig(1, second));
    EXPECT_EQ(second.pcmChannels, 12u);
    EXPECT_EQ(second.sourceChannelOffset, 12u);
}

} // namespace
