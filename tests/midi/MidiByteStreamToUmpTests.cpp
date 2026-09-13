// MidiByteStreamToUmpTests.cpp
// ASFW - WP-3 receive-direction conversion tests
//
// MIDI 1.0 byte stream -> UMP. Vectors are written against the byte stream and
// the expected words independently; nothing here is a round-trip through the
// transmit converter (see UmpRoundTripTests.cpp for that).
//
// Word layouts cross-checked against CoreMIDI MIDIMessages.h builders
// (MacOSX26.5.sdk): MIDI1UPChannelVoice :277, MIDI1UPSystemCommon :312,
// MIDI1UPSysEx :316-317.

#include <gtest/gtest.h>

#include <vector>

#include "Midi/Ump/MidiByteStreamToUmp.hpp"

using namespace ASFW::Midi::Ump;

namespace {

/// Feed a whole byte run and return every word produced.
std::vector<UmpWord> Convert(MidiByteStreamToUmp& parser,
                             std::vector<uint8_t> bytes) {
    std::vector<UmpWord> out(bytes.size() * MidiByteStreamToUmp::kMaxWordsPerByte + 4);
    const auto result = parser.Push(bytes, out);
    EXPECT_EQ(result.bytesConsumed, bytes.size());
    out.resize(result.wordsWritten);
    return out;
}

} // namespace

//==============================================================================
// Channel Voice
//==============================================================================

TEST(MidiByteStreamToUmp, NoteOnBecomesOneChannelVoiceWord) {
    MidiByteStreamToUmp parser;
    const auto words = Convert(parser, {0x92, 0x3C, 0x40});
    ASSERT_EQ(words.size(), 1u);
    // MT 0x2, group 0, status 0x9, channel 2, data 0x3C 0x40.
    EXPECT_EQ(words[0], 0x2092'3C40u);
}

TEST(MidiByteStreamToUmp, EmitsNothingUntilMessageIsComplete) {
    MidiByteStreamToUmp parser;
    EXPECT_TRUE(Convert(parser, {0x90}).empty());
    EXPECT_TRUE(Convert(parser, {0x40}).empty());
    const auto words = Convert(parser, {0x7F});
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0], 0x2090'407Fu);
}

TEST(MidiByteStreamToUmp, ProgramChangeTakesOneDataByte) {
    MidiByteStreamToUmp parser;
    const auto words = Convert(parser, {0xC5, 0x2A});
    ASSERT_EQ(words.size(), 1u);
    // Second data byte is zero-filled.
    EXPECT_EQ(words[0], 0x20C5'2A00u);
}

TEST(MidiByteStreamToUmp, ChannelPressureTakesOneDataByte) {
    MidiByteStreamToUmp parser;
    const auto words = Convert(parser, {0xD0, 0x55});
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0], 0x20D0'5500u);
}

TEST(MidiByteStreamToUmp, PitchBendCarriesBothDataBytes) {
    MidiByteStreamToUmp parser;
    const auto words = Convert(parser, {0xE3, 0x00, 0x40});
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0], 0x20E3'0040u);
}

TEST(MidiByteStreamToUmp, UsesConfiguredGroup) {
    MidiByteStreamToUmp parser{0x5};
    const auto words = Convert(parser, {0x90, 0x3C, 0x40});
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0], 0x2590'3C40u);
}

//==============================================================================
// Running status
//==============================================================================

TEST(MidiByteStreamToUmp, RunningStatusRepeatsTheLastChannelStatus) {
    MidiByteStreamToUmp parser;
    const auto words = Convert(parser, {0x90, 0x3C, 0x40, 0x3E, 0x50, 0x40, 0x60});
    ASSERT_EQ(words.size(), 3u);
    EXPECT_EQ(words[0], 0x2090'3C40u);
    EXPECT_EQ(words[1], 0x2090'3E50u);
    EXPECT_EQ(words[2], 0x2090'4060u);
}

TEST(MidiByteStreamToUmp, SystemCommonClearsRunningStatus) {
    MidiByteStreamToUmp parser;
    // Tune Request is System Common with no data bytes.
    auto words = Convert(parser, {0x90, 0x3C, 0x40, 0xF6});
    ASSERT_EQ(words.size(), 2u);
    EXPECT_EQ(words[1], 0x10F6'0000u);

    // The two bytes that follow have no status to inherit.
    words = Convert(parser, {0x3E, 0x50});
    EXPECT_TRUE(words.empty());
    EXPECT_EQ(parser.GetCounters().orphanDataBytes, 2u);
}

TEST(MidiByteStreamToUmp, RealTimeDoesNotClearRunningStatus) {
    MidiByteStreamToUmp parser;
    const auto words =
        Convert(parser, {0x90, 0x3C, 0x40, 0xF8, 0x3E, 0x50});
    ASSERT_EQ(words.size(), 3u);
    EXPECT_EQ(words[0], 0x2090'3C40u);
    EXPECT_EQ(words[1], 0x10F8'0000u);   // Timing Clock
    EXPECT_EQ(words[2], 0x2090'3E50u);   // still running status 0x90
}

TEST(MidiByteStreamToUmp, DataByteWithNoRunningStatusIsCounted) {
    MidiByteStreamToUmp parser;
    EXPECT_TRUE(Convert(parser, {0x40, 0x50}).empty());
    EXPECT_EQ(parser.GetCounters().orphanDataBytes, 2u);
}

//==============================================================================
// System Real Time interleaving
//==============================================================================

TEST(MidiByteStreamToUmp, RealTimeArrivingMidMessageDoesNotDisturbIt) {
    MidiByteStreamToUmp parser;
    // Timing Clock lands between the two data bytes of a Note On.
    const auto words = Convert(parser, {0x90, 0x3C, 0xF8, 0x40});
    ASSERT_EQ(words.size(), 2u);
    EXPECT_EQ(words[0], 0x10F8'0000u);   // real time emitted immediately
    EXPECT_EQ(words[1], 0x2090'3C40u);   // note on still completes correctly
}

TEST(MidiByteStreamToUmp, EveryRealTimeStatusIsForwarded) {
    for (uint8_t status = 0xF8; status != 0x00; ++status) {
        MidiByteStreamToUmp parser;
        const auto words = Convert(parser, {status});
        ASSERT_EQ(words.size(), 1u) << "status " << std::hex << int(status);
        EXPECT_EQ(words[0], 0x1000'0000u | (uint32_t(status) << 16));
    }
}

//==============================================================================
// System Common
//==============================================================================

TEST(MidiByteStreamToUmp, SongPositionPointerCarriesTwoDataBytes) {
    MidiByteStreamToUmp parser;
    const auto words = Convert(parser, {0xF2, 0x12, 0x34});
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0], 0x10F2'1234u);
}

TEST(MidiByteStreamToUmp, QuarterFrameAndSongSelectCarryOneDataByte) {
    MidiByteStreamToUmp parser;
    auto words = Convert(parser, {0xF1, 0x21});
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0], 0x10F1'2100u);

    words = Convert(parser, {0xF3, 0x07});
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0], 0x10F3'0700u);
}

TEST(MidiByteStreamToUmp, UndefinedSystemCommonIsDroppedAndCounted) {
    MidiByteStreamToUmp parser;
    EXPECT_TRUE(Convert(parser, {0xF4}).empty());
    EXPECT_TRUE(Convert(parser, {0xF5}).empty());
    EXPECT_EQ(parser.GetCounters().unsupportedStatusBytes, 2u);
}

TEST(MidiByteStreamToUmp, StandaloneEndOfExclusiveIsDroppedAndCounted) {
    MidiByteStreamToUmp parser;
    EXPECT_TRUE(Convert(parser, {0xF7}).empty());
    EXPECT_EQ(parser.GetCounters().unsupportedStatusBytes, 1u);
}

//==============================================================================
// System Exclusive
//==============================================================================

TEST(MidiByteStreamToUmp, EmptySysExIsOneCompletePacketWithNoPayload) {
    MidiByteStreamToUmp parser;
    const auto words = Convert(parser, {0xF0, 0xF7});
    ASSERT_EQ(words.size(), 2u);
    // MT 0x3, group 0, status Complete (0x0), numBytes 0.
    EXPECT_EQ(words[0], 0x3000'0000u);
    EXPECT_EQ(words[1], 0x0000'0000u);
}

TEST(MidiByteStreamToUmp, ShortSysExIsOneCompletePacket) {
    MidiByteStreamToUmp parser;
    const auto words = Convert(parser, {0xF0, 0x41, 0x10, 0x42, 0xF7});
    ASSERT_EQ(words.size(), 2u);
    EXPECT_EQ(words[0], 0x3003'4110u);   // Complete, 3 bytes, 0x41 0x10
    EXPECT_EQ(words[1], 0x4200'0000u);   // 0x42 then zero fill
}

TEST(MidiByteStreamToUmp, ExactlySixPayloadBytesStaysOnePacket) {
    MidiByteStreamToUmp parser;
    const auto words =
        Convert(parser, {0xF0, 1, 2, 3, 4, 5, 6, 0xF7});
    ASSERT_EQ(words.size(), 2u);
    EXPECT_EQ(words[0], 0x3006'0102u);   // Complete, 6 bytes
    EXPECT_EQ(words[1], 0x0304'0506u);
}

TEST(MidiByteStreamToUmp, SevenPayloadBytesSplitIntoStartAndEnd) {
    MidiByteStreamToUmp parser;
    const auto words =
        Convert(parser, {0xF0, 1, 2, 3, 4, 5, 6, 7, 0xF7});
    ASSERT_EQ(words.size(), 4u);
    EXPECT_EQ(words[0], 0x3016'0102u);   // Start, 6 bytes
    EXPECT_EQ(words[1], 0x0304'0506u);
    EXPECT_EQ(words[2], 0x3031'0700u);   // End, 1 byte
    EXPECT_EQ(words[3], 0x0000'0000u);
}

TEST(MidiByteStreamToUmp, ThirteenPayloadBytesProduceStartContinueEnd) {
    MidiByteStreamToUmp parser;
    std::vector<uint8_t> stream{0xF0};
    for (uint8_t i = 1; i <= 13; ++i) stream.push_back(i);
    stream.push_back(0xF7);

    const auto words = Convert(parser, stream);
    ASSERT_EQ(words.size(), 6u);
    EXPECT_EQ(words[0], 0x3016'0102u);   // Start
    EXPECT_EQ(words[2], 0x3026'0708u);   // Continue
    EXPECT_EQ(words[4], 0x3031'0D00u);   // End, 1 byte (0x0D == 13)
}

TEST(MidiByteStreamToUmp, RealTimeInsideSysExPassesThroughWithoutBreakingIt) {
    MidiByteStreamToUmp parser;
    const auto words =
        Convert(parser, {0xF0, 0x41, 0xFE, 0x10, 0x42, 0xF7});
    ASSERT_EQ(words.size(), 3u);
    EXPECT_EQ(words[0], 0x10FE'0000u);   // Active Sensing, emitted immediately
    EXPECT_EQ(words[1], 0x3003'4110u);   // SysEx still sees 3 payload bytes
    EXPECT_EQ(words[2], 0x4200'0000u);
}

TEST(MidiByteStreamToUmp, StatusByteInterruptingSysExTerminatesItAndCounts) {
    MidiByteStreamToUmp parser;
    const auto words =
        Convert(parser, {0xF0, 0x41, 0x10, 0x90, 0x3C, 0x40});
    ASSERT_EQ(words.size(), 3u);
    EXPECT_EQ(words[0], 0x3002'4110u);   // truncated SysEx closed as Complete
    EXPECT_EQ(words[1], 0x0000'0000u);
    EXPECT_EQ(words[2], 0x2090'3C40u);   // the interrupting Note On still works
    EXPECT_EQ(parser.GetCounters().sysExTruncated, 1u);
    EXPECT_FALSE(parser.InSysEx());
}

TEST(MidiByteStreamToUmp, NewSysExWhileOneIsOpenTerminatesThePrevious) {
    MidiByteStreamToUmp parser;
    const auto words = Convert(parser, {0xF0, 0x41, 0xF0, 0x42, 0xF7});
    ASSERT_EQ(words.size(), 4u);
    EXPECT_EQ(words[0], 0x3001'4100u);   // first closed as Complete, 1 byte
    EXPECT_EQ(words[2], 0x3001'4200u);   // second completes normally
    EXPECT_EQ(parser.GetCounters().sysExTruncated, 1u);
}

TEST(MidiByteStreamToUmp, SysExClearsRunningStatus) {
    MidiByteStreamToUmp parser;
    Convert(parser, {0x90, 0x3C, 0x40});
    Convert(parser, {0xF0, 0x41, 0xF7});
    const auto words = Convert(parser, {0x3E, 0x50});
    EXPECT_TRUE(words.empty());
    EXPECT_EQ(parser.GetCounters().orphanDataBytes, 2u);
}

//==============================================================================
// Multi-byte delivery: one AM824 quadlet can carry up to three bytes
//==============================================================================

TEST(MidiByteStreamToUmp, ThreeByteRunInOneCallCompletesAMessage) {
    MidiByteStreamToUmp parser;
    const uint8_t quadletPayload[3] = {0x90, 0x3C, 0x40};
    UmpWord out[8]{};
    const auto result = parser.Push(quadletPayload, out);
    EXPECT_EQ(result.bytesConsumed, 3u);
    ASSERT_EQ(result.wordsWritten, 1u);
    EXPECT_EQ(out[0], 0x2090'3C40u);
}

TEST(MidiByteStreamToUmp, MessageSplitAcrossCallsStillCompletes) {
    MidiByteStreamToUmp parser;
    UmpWord out[8]{};
    const uint8_t first[2] = {0x90, 0x3C};
    auto result = parser.Push(first, out);
    EXPECT_EQ(result.wordsWritten, 0u);

    const uint8_t second[1] = {0x40};
    result = parser.Push(second, out);
    ASSERT_EQ(result.wordsWritten, 1u);
    EXPECT_EQ(out[0], 0x2090'3C40u);
}

//==============================================================================
// Output space
//==============================================================================

TEST(MidiByteStreamToUmp, StopsConsumingWhenOutputCannotHoldAWholePacket) {
    MidiByteStreamToUmp parser;
    // One word of room: not enough to guarantee a two-word SysEx packet, so
    // nothing is consumed at all.
    UmpWord out[1]{};
    const uint8_t stream[3] = {0x90, 0x3C, 0x40};
    const auto result = parser.Push(stream, out);
    EXPECT_EQ(result.bytesConsumed, 0u);
    EXPECT_EQ(result.wordsWritten, 0u);
}

TEST(MidiByteStreamToUmp, RemainderIsConsumedOnTheNextCall) {
    MidiByteStreamToUmp parser;
    std::vector<uint8_t> stream{0xF0, 1, 2, 3, 4, 5, 6, 7, 0xF7};
    // Room for one packet only.
    UmpWord out[2]{};
    auto result = parser.Push(stream, out);
    EXPECT_EQ(result.wordsWritten, 2u);
    EXPECT_EQ(out[0], 0x3016'0102u);   // Start
    ASSERT_LT(result.bytesConsumed, stream.size());

    const std::span<const uint8_t> rest{stream.data() + result.bytesConsumed,
                                        stream.size() - result.bytesConsumed};
    UmpWord out2[8]{};
    result = parser.Push(rest, out2);
    EXPECT_EQ(result.bytesConsumed, rest.size());
    ASSERT_EQ(result.wordsWritten, 2u);
    EXPECT_EQ(out2[0], 0x3031'0700u);  // End
}

//==============================================================================
// Reset and stream loss
//==============================================================================

TEST(MidiByteStreamToUmp, ResetDiscardsAPartialChannelMessage) {
    MidiByteStreamToUmp parser;
    EXPECT_TRUE(Convert(parser, {0x90, 0x3C}).empty());
    parser.Reset();
    EXPECT_EQ(parser.GetCounters().partialMessagesDiscarded, 1u);

    // The orphaned data byte must not complete the pre-reset message.
    const auto words = Convert(parser, {0x40});
    EXPECT_TRUE(words.empty());
    EXPECT_EQ(parser.GetCounters().orphanDataBytes, 1u);
}

TEST(MidiByteStreamToUmp, ResetDiscardsRunningStatus) {
    MidiByteStreamToUmp parser;
    Convert(parser, {0x90, 0x3C, 0x40});
    parser.Reset();
    EXPECT_TRUE(Convert(parser, {0x3E, 0x50}).empty());
    EXPECT_EQ(parser.GetCounters().orphanDataBytes, 2u);
}

TEST(MidiByteStreamToUmp, ResetDropsAnOpenSysExWithoutEmittingIt) {
    MidiByteStreamToUmp parser;
    EXPECT_TRUE(Convert(parser, {0xF0, 0x41, 0x10}).empty());
    EXPECT_TRUE(parser.InSysEx());

    parser.Reset();
    EXPECT_FALSE(parser.InSysEx());
    EXPECT_EQ(parser.GetCounters().partialMessagesDiscarded, 1u);
    // Reset emits nothing: a dropped SysEx must not be reported as complete.
    EXPECT_EQ(parser.GetCounters().wordsEmitted, 0u);
}

TEST(MidiByteStreamToUmp, BytesAcrossALossGapCannotFormAMessage) {
    MidiByteStreamToUmp parser;
    // First half of a Note On, then the stream is lost.
    Convert(parser, {0x90, 0x3C});
    parser.Reset();
    // A stale second half plus a stale first half must not join up.
    const auto words = Convert(parser, {0x40, 0x3E});
    EXPECT_TRUE(words.empty());
}
