// SPDX-License-Identifier: Apache-2.0
//
// MotuPortLayout tests.
//
// Pin which jack each host channel reaches. The chunk positions come from FFADO's
// port-group tables (motu_avdevice.cpp:111-121, :135-145), whose packet offsets are
// assigned in array order (motu_avdevice.cpp:1839-1862).

#include "Audio/Wire/MOTU/MotuPortLayout.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

namespace {

using ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion;
using ASFW::DeviceProfiles::Audio::kMotu896hdSwVersion;
using ASFW::DeviceProfiles::Audio::kMotuUltraliteSwVersion;
using ASFW::Encoding::Motu::CapturePortsForSwVersion;
using ASFW::Encoding::Motu::ChunkForHostChannel;
using ASFW::Encoding::Motu::EffectivePortMap;
using ASFW::Encoding::Motu::IsChunkPermutation;
using ASFW::Encoding::Motu::MotuPort;
using ASFW::Encoding::Motu::MotuPortMap;
using ASFW::Encoding::Motu::PlaybackPortsForSwVersion;

constexpr uint32_t kFixedChunks = 14;

TEST(MotuPortLayoutTests, UltraLiteDefaultOutputPairIsTheMainOuts) {
    const MotuPortMap playback = PlaybackPortsForSwVersion(kMotuUltraliteSwVersion);
    ASSERT_EQ(playback.size(), kFixedChunks);
    // Wire chunks 10-11 are Main L/R; chunks 0-1 are the headphone pair.
    EXPECT_EQ(ChunkForHostChannel(playback, 0), 10U);
    EXPECT_EQ(ChunkForHostChannel(playback, 1), 11U);
    EXPECT_EQ(std::string_view(playback[0].name), "Main L");
    EXPECT_EQ(std::string_view(playback[1].name), "Main R");
    EXPECT_EQ(ChunkForHostChannel(playback, 12), 0U);
    EXPECT_EQ(std::string_view(playback[12].name), "Phones L");
}

TEST(MotuPortLayoutTests, UltraLiteDefaultInputPairIsTheMicInputs) {
    const MotuPortMap capture = CapturePortsForSwVersion(kMotuUltraliteSwVersion);
    ASSERT_EQ(capture.size(), kFixedChunks);
    // Wire chunks 2-3 are Mic 1/2; chunks 0-1 are the CueMix return.
    EXPECT_EQ(ChunkForHostChannel(capture, 0), 2U);
    EXPECT_EQ(ChunkForHostChannel(capture, 1), 3U);
    EXPECT_EQ(std::string_view(capture[0].name), "Mic 1");
    EXPECT_EQ(ChunkForHostChannel(capture, 10), 0U);
}

TEST(MotuPortLayoutTests, The828mk2MicsSitAfterItsEightAnalogInputs) {
    const MotuPortMap capture = CapturePortsForSwVersion(kMotu828mk2SwVersion);
    ASSERT_EQ(capture.size(), kFixedChunks);
    EXPECT_EQ(ChunkForHostChannel(capture, 0), 2U);  // Analog 1
    EXPECT_EQ(ChunkForHostChannel(capture, 8), 10U); // Mic 1
    EXPECT_EQ(std::string_view(capture[8].name), "Mic 1");
}

TEST(MotuPortLayoutTests, EveryModelTableIsAPermutationOfItsChunks) {
    for (const uint32_t sw : {kMotu828mk2SwVersion, kMotuUltraliteSwVersion}) {
        EXPECT_TRUE(IsChunkPermutation(PlaybackPortsForSwVersion(sw))) << sw;
        EXPECT_TRUE(IsChunkPermutation(CapturePortsForSwVersion(sw))) << sw;
    }
}

TEST(MotuPortLayoutTests, UnconfirmedModelsStayInWireOrder) {
    EXPECT_TRUE(PlaybackPortsForSwVersion(kMotu896hdSwVersion).empty());
    EXPECT_TRUE(CapturePortsForSwVersion(kMotu896hdSwVersion).empty());
    EXPECT_EQ(ChunkForHostChannel({}, 5), 5U);
}

TEST(MotuPortLayoutTests, ChannelsPastTheTableKeepTheirWireChunk) {
    // Optical extras follow every fixed chunk, so a 22-chunk ADAT stream maps
    // host channel 14+ straight through.
    const MotuPortMap playback = PlaybackPortsForSwVersion(kMotu828mk2SwVersion);
    const MotuPortMap map = EffectivePortMap(playback, 22);
    EXPECT_EQ(map.size(), kFixedChunks);
    EXPECT_EQ(ChunkForHostChannel(map, 14), 14U);
    EXPECT_EQ(ChunkForHostChannel(map, 21), 21U);
}

TEST(MotuPortLayoutTests, ATableWiderThanTheStreamFallsBackToWireOrder) {
    // Chunk 13 does not exist in a 12-chunk stream, so the table cannot apply.
    const MotuPortMap playback = PlaybackPortsForSwVersion(kMotuUltraliteSwVersion);
    EXPECT_TRUE(EffectivePortMap(playback, 12).empty());
}

TEST(MotuPortLayoutTests, PermutationCheckRejectsDuplicatesAndGaps) {
    constexpr MotuPort duplicate[] = {{"a", 0}, {"b", 0}};
    constexpr MotuPort gap[] = {{"a", 0}, {"b", 2}};
    EXPECT_FALSE(IsChunkPermutation(duplicate));
    EXPECT_FALSE(IsChunkPermutation(gap));
}

} // namespace
