// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The geometry the 1814 and ProjectMix cannot be asked for. Every expectation
// here is transcribed from bebob_maudio.c:227-254 — if this table is wrong the
// stream shape is silently wrong on the wire, with no error anywhere, so the
// tests are written as a literal restatement of the reference rather than as
// properties derived from it.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/BeBoB/MAudioSpecialFormation.hpp"

using namespace ASFW::Audio::BeBoB;

namespace {

constexpr auto kSpdif = MAudioDigitalFormat::SPDIF;
constexpr auto kAdat = MAudioDigitalFormat::ADAT;

} // namespace

TEST(MAudioSpecialFormationTests, SpdifMatchesTheReferenceTable) {
    // ch_table[IN][0] = {10,10,2}, ch_table[OUT][0] = {6,6,4}
    struct Expected { uint32_t rate; uint32_t capture; uint32_t playback; };
    constexpr Expected kRows[] = {
        { 44100, 10, 6}, { 48000, 10, 6},
        { 88200, 10, 6}, { 96000, 10, 6},
        {176400,  2, 4}, {192000,  2, 4},
    };
    for (const auto& row : kRows) {
        const auto formation = MAudioFormationFor(kSpdif, kSpdif, row.rate);
        ASSERT_TRUE(formation.has_value()) << row.rate;
        EXPECT_EQ(formation->capturePcmChannels, row.capture) << row.rate;
        EXPECT_EQ(formation->playbackPcmChannels, row.playback) << row.rate;
        EXPECT_EQ(formation->midiDataBlocks, 1U) << row.rate;
    }
}

TEST(MAudioSpecialFormationTests, AdatMatchesTheReferenceTable) {
    // ch_table[IN][1] = {16,12,2}, ch_table[OUT][1] = {12,8,4}
    struct Expected { uint32_t rate; uint32_t capture; uint32_t playback; };
    constexpr Expected kRows[] = {
        { 44100, 16, 12}, { 48000, 16, 12},
        { 88200, 12,  8}, { 96000, 12,  8},
        {176400,  2,  4}, {192000,  2,  4},
    };
    for (const auto& row : kRows) {
        const auto formation = MAudioFormationFor(kAdat, kAdat, row.rate);
        ASSERT_TRUE(formation.has_value()) << row.rate;
        EXPECT_EQ(formation->capturePcmChannels, row.capture) << row.rate;
        EXPECT_EQ(formation->playbackPcmChannels, row.playback) << row.rate;
    }
}

TEST(MAudioSpecialFormationTests, CaptureAndPlaybackAreSelectedIndependently) {
    // dig_in_fmt indexes capture, dig_out_fmt indexes playback, so a mixed
    // combination is legal. Collapsing them into one "mode" would silently
    // mis-shape one direction.
    const auto mixed = MAudioFormationFor(kSpdif, kAdat, 48000);
    ASSERT_TRUE(mixed.has_value());
    EXPECT_EQ(mixed->capturePcmChannels, 10U);   // S/PDIF in
    EXPECT_EQ(mixed->playbackPcmChannels, 12U);  // ADAT out

    const auto opposite = MAudioFormationFor(kAdat, kSpdif, 48000);
    ASSERT_TRUE(opposite.has_value());
    EXPECT_EQ(opposite->capturePcmChannels, 16U);
    EXPECT_EQ(opposite->playbackPcmChannels, 6U);
}

TEST(MAudioSpecialFormationTests, RatesPairIntoBandsTheWayLinuxIndexesThem) {
    // Linux writes ch_table[...][i / 2] over formation entries 1..6, so each
    // band covers two adjacent rates. 44.1 and 48 must resolve identically, and
    // 96 must differ from 88.2's neighbour band boundary rather than from 88.2.
    EXPECT_EQ(MAudioRateBand(44100), MAudioRateBand(48000));
    EXPECT_EQ(MAudioRateBand(88200), MAudioRateBand(96000));
    EXPECT_EQ(MAudioRateBand(176400), MAudioRateBand(192000));
    EXPECT_NE(MAudioRateBand(48000), MAudioRateBand(88200));
    EXPECT_NE(MAudioRateBand(96000), MAudioRateBand(176400));
}

TEST(MAudioSpecialFormationTests, RejectsRatesThisFamilyNeverRuns) {
    // 32 kHz is in the generic BeBoB rate table as formation entry 0, which
    // special_stream_formation_set never writes — the loop starts at i+1.
    EXPECT_FALSE(MAudioRateBand(32000).has_value());
    EXPECT_FALSE(MAudioFormationFor(kSpdif, kSpdif, 32000).has_value());
    EXPECT_FALSE(MAudioFormationFor(kSpdif, kSpdif, 0).has_value());
    EXPECT_FALSE(MAudioFormationFor(kSpdif, kSpdif, 47999).has_value());
}

TEST(MAudioSpecialFormationTests, ProjectMixStopsTwoRatesShortOfThe1814) {
    // Linux expresses the ProjectMix difference as `max -= 2` on the same
    // table, not as a different table — so the geometry is shared and only the
    // offered rate list differs.
    EXPECT_EQ(kMAudioFireWire1814RateCount, 6U);
    EXPECT_EQ(kMAudioProjectMixRateCount, 4U);
    EXPECT_EQ(kMAudioSpecialRatesHz[kMAudioProjectMixRateCount - 1], 96000U);
    EXPECT_EQ(kMAudioSpecialRatesHz[kMAudioFireWire1814RateCount - 1], 192000U);
}

TEST(MAudioSpecialFormationTests, HighRateBandCarriesMorePlaybackThanCapture) {
    // Worth pinning because it inverts every other band and looks like a
    // transcription slip: at 176.4/192 capture drops to 2 while playback holds
    // at 4, in BOTH digital formats.
    for (const auto format : {kSpdif, kAdat}) {
        for (const uint32_t rate : {176400U, 192000U}) {
            const auto formation = MAudioFormationFor(format, format, rate);
            ASSERT_TRUE(formation.has_value());
            EXPECT_EQ(formation->capturePcmChannels, 2U);
            EXPECT_EQ(formation->playbackPcmChannels, 4U);
        }
    }
}
