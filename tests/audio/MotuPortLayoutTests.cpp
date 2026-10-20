// SPDX-License-Identifier: Apache-2.0
//
// MotuPortLayout tests.
//
// Pin which jack each host channel reaches. The chunk positions come from FFADO's
// port-group tables (motu_avdevice.cpp:111-121, :135-145), whose packet offsets are
// assigned in array order (motu_avdevice.cpp:1839-1862).

#include "Audio/Wire/MOTU/MotuPortLayout.hpp"
#include "Audio/DriverKit/Config/MOTU/MotuProfile.hpp"
#include "MotuChannelMappingFixtures.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

namespace {

using ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion;
using ASFW::DeviceProfiles::Audio::kMotu896hdSwVersion;
using ASFW::DeviceProfiles::Audio::kMotuUltraliteSwVersion;
using ASFW::Encoding::Motu::ChunkForHostChannel;
using ASFW::Encoding::Motu::EffectivePortMap;
using ASFW::Encoding::Motu::IsChunkPermutation;
using ASFW::Encoding::Motu::MotuPort;
using ASFW::Encoding::Motu::MotuPortMap;
using ASFW::Isoch::Audio::MOTU::Profiles::CapturePortsForSwVersion;
using ASFW::Isoch::Audio::MOTU::Profiles::PlaybackPortsForSwVersion;

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

TEST(MotuPortLayoutTests, The896HdUsesItsReferenceMainAndAnalogChunks) {
    EXPECT_EQ(PlaybackPortsForSwVersion(kMotu896hdSwVersion)[0].chunk, 10U);
    EXPECT_EQ(CapturePortsForSwVersion(kMotu896hdSwVersion)[0].chunk, 2U);
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

TEST(MotuPortLayoutTests, UltraLiteMk3MatchesTheReportedJacksAndVendorOrder) {
    const auto capture = CapturePortsForSwVersion(0x19, 48000, 18);
    const auto playback = PlaybackPortsForSwVersion(0x19, 48000, 14);
    const std::array<uint8_t, 14> expectedCapture{2,3,4,5,6,7,8,9,10,11,0,1,14,15};
    const std::array<uint8_t, 14> expectedPlayback{0,1,2,3,4,5,6,7,8,9,12,13,10,11};
    for (uint32_t i = 0; i < 14; ++i) {
        EXPECT_EQ(capture[i].chunk, expectedCapture[i]);
        EXPECT_EQ(playback[i].chunk, expectedPlayback[i]);
    }
    EXPECT_EQ(ASFW::Encoding::Motu::HostChannelsFor(0x19,48000,18,true),14U);
    EXPECT_EQ(ASFW::Encoding::Motu::HostChannelsFor(0x19,96000,14,true),12U);
    EXPECT_EQ(ASFW::Encoding::Motu::HostChannelsFor(0x19,192000,10,true),10U);
    EXPECT_EQ(ASFW::Encoding::Motu::HostChannelsFor(0x19,192000,14,false),12U);
    EXPECT_EQ(std::string_view(playback[10].name),"S/PDIF 1");
    EXPECT_EQ(std::string_view(playback[12].name),"Phones L");
}

TEST(MotuPortLayoutTests, EightPreOpticalChannelsMoveAheadOfWirePadding) {
    const auto playback = PlaybackPortsForSwVersion(0xf,48000,14);
    const std::array<uint8_t,14> expected{2,3,0,1,6,7,8,9,10,11,12,13,4,5};
    for (uint32_t i=0; i<expected.size(); ++i) EXPECT_EQ(playback[i].chunk,expected[i]);
    EXPECT_EQ(ASFW::Encoding::Motu::HostChannelsFor(0xf,48000,14,false),12U);
}

TEST(MotuPortLayoutTests, The896HdVendorAesPairPrecedesOpticalOnTheWire) {
    const auto capture = CapturePortsForSwVersion(5,48000,22);
    const auto playback = PlaybackPortsForSwVersion(5,48000,22);
    EXPECT_EQ(capture[8].chunk,10U); EXPECT_EQ(capture[9].chunk,11U);
    EXPECT_EQ(playback[10].chunk,12U); EXPECT_EQ(playback[11].chunk,13U);
    EXPECT_EQ(PlaybackPortsForSwVersion(5,192000,8)[0].chunk,0U);
}

TEST(MotuPortLayoutTests, The828Mk3HighRateDoesNotReuseMainOutLabels) {
    const auto playback = PlaybackPortsForSwVersion(0x15,192000,10);
    ASSERT_EQ(playback.size(),10U);
    EXPECT_EQ(playback[0].chunk,2U);
    EXPECT_EQ(std::string_view(playback[0].name),"Analog 1");
    EXPECT_EQ(std::string_view(playback[8].name),"Unknown");
}

TEST(MotuPortLayoutTests, AllSupportedFixedAndOpticalLayoutsAreCompletePermutations) {
    using namespace ASFW::Encoding::Motu;
    for (const auto& model : kModels) {
        if (!FireWireOnly(model.unitVersion)) continue;
        for (uint32_t rate : kClockRates) {
            if (!SupportsRate(model,rate)) continue;
            const auto mode=IndexToMode(static_cast<uint32_t>(RateToIndex(rate)));
            for (bool capture : {false,true}) {
                const auto fixed=capture ? model.captureChunks[mode] : model.playbackChunks[mode];
                for (uint32_t extra : {0U,4U,8U,12U,16U}) {
                    if (fixed+extra>34) continue;
                    const auto* layout=PortLayoutFor(model.unitVersion,rate,fixed+extra,capture);
                    ASSERT_NE(layout,nullptr) << model.name << " rate=" << rate;
                    EXPECT_TRUE(IsChunkPermutation(layout->Map())) << model.name << " rate=" << rate << " capture=" << capture << " extra=" << extra;
                    EXPECT_LE(layout->hostChannels,layout->wireChunks);
                }
            }
        }
    }
}

TEST(MotuPortLayoutTests, TravelerSharedDigitalPairDoesNotClaimOnlyCoaxSpdif) {
    const auto ports=CapturePortsForSwVersion(9,48000,14);
    EXPECT_EQ(ports[10].chunk,12U);
    EXPECT_EQ(std::string_view(ports[10].name),"Digital 1");
}

TEST(MotuPortLayoutTests, EverySupportedModelNamesTheVendorPhysicalGroupsAtEveryFormation) {
    using namespace MotuChannelFixtures;
    std::array<bool,kModels.size()> covered{};
    for (bool capture : {false,true}) EachFormation(capture,
        [&](const Model& model,uint32_t rate,uint32_t mode,uint32_t wire,uint32_t extra) {
            SCOPED_TRACE(std::string(model.name)+" rate="+std::to_string(rate)+
                         " capture="+std::to_string(capture)+" wire="+std::to_string(wire));
            const auto* layout = PortLayoutFor(model.unitVersion,rate,wire,capture);
            ASSERT_NE(layout,nullptr);
            EXPECT_TRUE(IsChunkPermutation(layout->Map()));
            bool hasEvidence = false;
            for (const auto& group : groups) {
                if (group.version != model.unitVersion || group.capture != capture) continue;
                hasEvidence = true;
                for (size_t i=0;i<group.names.size();++i) {
                    const auto visible = MotuPortMap{layout->ports.data(),layout->hostChannels};
                    const auto found = std::ranges::find_if(visible,
                        [&](const auto& port) { return port.name == group.names[i]; });
                    if (group.firstChunk[mode] < 0) { EXPECT_EQ(found,visible.end()) << group.names[i]; continue; }
                    ASSERT_NE(found,visible.end()) << group.names[i];
                    EXPECT_EQ(found->chunk,group.firstChunk[mode]+i+(group.followsOptical ? extra : 0))
                        << group.names[i];
                }
            }
            EXPECT_TRUE(hasEvidence);
            const auto it = std::ranges::find_if(kModels,[&](const auto& item) { return item.unitVersion == model.unitVersion; });
            covered[static_cast<size_t>(it-kModels.begin())] = true;
        });
    EXPECT_EQ(std::ranges::count(covered,true),12);
}

// Vendor BoxTrack16::SetupStrmFwIds 0x51330: one optical bank (bank-B bits ignored),
// a two-chunk gap before Reverb, and no Reverb or optical at 4x.
TEST(MotuPortLayoutTests, Track16UsesVendorGroupsAndOnlyOpticalBankA) {
    using namespace ASFW::Encoding::Motu;
    using ASFW::Audio::Motu::ResolvePcmChunks;
    EXPECT_EQ(ResolvePcmChunks(0x101, 0, 0x39).tx, 22U);
    EXPECT_EQ(ResolvePcmChunks(0x101, 0, 0x39).rx, 14U);
    EXPECT_EQ(ResolvePcmChunks(0x10101 | 0x40000, 0, 0x39).tx, 18U); // TOSLINK: 2 + 2 padding
    EXPECT_EQ(ResolvePcmChunks(0x10101 | 0x40000, 0, 0x39).rx, 10U);
    EXPECT_EQ(ResolvePcmChunks(0x101, 1, 0x39).tx, 18U);
    EXPECT_EQ(ResolvePcmChunks(0x202, 0, 0x39).tx, 14U);
    EXPECT_EQ(ResolvePcmChunks(0x303, 2, 0x39).tx, 14U);

    const auto* capture = PortLayoutFor(0x39, 48000, 14, true);
    ASSERT_NE(capture, nullptr);
    EXPECT_EQ(capture->hostChannels, 12U);
    EXPECT_EQ(std::string_view(capture->ports[10].name), "Reverb L");
    EXPECT_EQ(capture->ports[10].chunk, 12U);
    EXPECT_EQ(capture->ports[12].chunk, 10U); // gap, hidden
    EXPECT_EQ(capture->ports[13].chunk, 11U);

    const auto* quad = PortLayoutFor(0x39, 192000, 14, true);
    ASSERT_NE(quad, nullptr);
    EXPECT_EQ(quad->hostChannels, 10U);
    EXPECT_TRUE(IsChunkPermutation(quad->Map()));

    const auto* playback = PortLayoutFor(0x39, 48000, 14, false);
    ASSERT_NE(playback, nullptr);
    EXPECT_EQ(playback->hostChannels, 14U);
    EXPECT_EQ(std::string_view(playback->ports[0].name), "Main L");
    EXPECT_EQ(std::string_view(playback->ports[6].name), "Optical 1");
    EXPECT_EQ(playback->ports[6].chunk, 6U);
}
