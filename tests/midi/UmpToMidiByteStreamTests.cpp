// UmpToMidiByteStreamTests.cpp
// ASFW - WP-3 transmit-direction conversion tests
//
// UMP -> MIDI 1.0 byte stream. Input words are written by hand against the
// layouts in CoreMIDI MIDIMessages.h, not produced by the receive converter.

#include <gtest/gtest.h>

#include <vector>

#include "Midi/Ump/UmpToMidiByteStream.hpp"

using namespace ASFW::Midi::Ump;

namespace {

std::vector<uint8_t> Convert(UmpToMidiByteStream& writer,
                             std::vector<UmpWord> words,
                             bool expectAllConsumed = true) {
    std::vector<uint8_t> out(
        words.size() * UmpToMidiByteStream::kMaxBytesPerPacket + 8);
    const auto result = writer.Pull(words, out);
    if (expectAllConsumed) EXPECT_EQ(result.wordsConsumed, words.size());
    out.resize(result.bytesWritten);
    return out;
}

} // namespace

//==============================================================================
// Channel Voice
//==============================================================================

TEST(UmpToMidiByteStream, NoteOnBecomesThreeBytes) {
    UmpToMidiByteStream writer;
    EXPECT_EQ(Convert(writer, {0x2092'3C40u}),
              (std::vector<uint8_t>{0x92, 0x3C, 0x40}));
}

TEST(UmpToMidiByteStream, ProgramChangeEmitsOnlyOneDataByte) {
    UmpToMidiByteStream writer;
    // data2 is set in the word and must be dropped, not emitted.
    EXPECT_EQ(Convert(writer, {0x20C5'2A7Fu}),
              (std::vector<uint8_t>{0xC5, 0x2A}));
}

TEST(UmpToMidiByteStream, ChannelPressureEmitsOnlyOneDataByte) {
    UmpToMidiByteStream writer;
    EXPECT_EQ(Convert(writer, {0x20D0'5500u}),
              (std::vector<uint8_t>{0xD0, 0x55}));
}

TEST(UmpToMidiByteStream, RepeatedMessagesKeepTheirStatusByte) {
    UmpToMidiByteStream writer;
    // No running-status compression: three full messages, nine bytes.
    EXPECT_EQ(Convert(writer, {0x2090'3C40u, 0x2090'3E50u, 0x2090'4060u}),
              (std::vector<uint8_t>{0x90, 0x3C, 0x40,
                                    0x90, 0x3E, 0x50,
                                    0x90, 0x40, 0x60}));
}

TEST(UmpToMidiByteStream, DataByteWithBitSevenSetRejectsThePacket) {
    UmpToMidiByteStream writer;
    // 0x80 in a data field would read as a status byte on the wire.
    EXPECT_TRUE(Convert(writer, {0x2090'8040u}).empty());
    EXPECT_EQ(writer.GetCounters().malformedPackets, 1u);
}

TEST(UmpToMidiByteStream, SecondDataByteIsOnlyCheckedWhenItIsUsed) {
    UmpToMidiByteStream writer;
    // Program Change ignores data2, so a dirty data2 must not reject it.
    EXPECT_EQ(Convert(writer, {0x20C5'2AFFu}),
              (std::vector<uint8_t>{0xC5, 0x2A}));
    EXPECT_EQ(writer.GetCounters().malformedPackets, 0u);
}

TEST(UmpToMidiByteStream, SystemStatusNibbleInChannelVoiceIsRejected) {
    UmpToMidiByteStream writer;
    // Status nibble 0xF would encode a System message, which is MT 0x1.
    EXPECT_TRUE(Convert(writer, {0x20F0'0000u}).empty());
    EXPECT_EQ(writer.GetCounters().malformedPackets, 1u);
}

//==============================================================================
// System
//==============================================================================

TEST(UmpToMidiByteStream, RealTimeIsOneByte) {
    UmpToMidiByteStream writer;
    EXPECT_EQ(Convert(writer, {0x10F8'0000u}), (std::vector<uint8_t>{0xF8}));
    EXPECT_EQ(Convert(writer, {0x10FF'0000u}), (std::vector<uint8_t>{0xFF}));
}

TEST(UmpToMidiByteStream, SongPositionPointerEmitsBothDataBytes) {
    UmpToMidiByteStream writer;
    EXPECT_EQ(Convert(writer, {0x10F2'1234u}),
              (std::vector<uint8_t>{0xF2, 0x12, 0x34}));
}

TEST(UmpToMidiByteStream, SongSelectEmitsOneDataByte) {
    UmpToMidiByteStream writer;
    EXPECT_EQ(Convert(writer, {0x10F3'0700u}),
              (std::vector<uint8_t>{0xF3, 0x07}));
}

TEST(UmpToMidiByteStream, TuneRequestEmitsStatusOnly) {
    UmpToMidiByteStream writer;
    EXPECT_EQ(Convert(writer, {0x10F6'0000u}), (std::vector<uint8_t>{0xF6}));
}

TEST(UmpToMidiByteStream, UndefinedSystemCommonIsRejected) {
    UmpToMidiByteStream writer;
    EXPECT_TRUE(Convert(writer, {0x10F4'0000u}).empty());
    EXPECT_TRUE(Convert(writer, {0x10F5'0000u}).empty());
    EXPECT_EQ(writer.GetCounters().malformedPackets, 2u);
}

TEST(UmpToMidiByteStream, SysExFramingAsSystemStatusIsRejected) {
    UmpToMidiByteStream writer;
    // 0xF0 and 0xF7 belong to MT 0x3, never MT 0x1.
    EXPECT_TRUE(Convert(writer, {0x10F0'0000u}).empty());
    EXPECT_TRUE(Convert(writer, {0x10F7'0000u}).empty());
    EXPECT_EQ(writer.GetCounters().malformedPackets, 2u);
}

TEST(UmpToMidiByteStream, NonSystemStatusInSystemPacketIsRejected) {
    UmpToMidiByteStream writer;
    EXPECT_TRUE(Convert(writer, {0x1090'0000u}).empty());
    EXPECT_EQ(writer.GetCounters().malformedPackets, 1u);
}

//==============================================================================
// System Exclusive
//==============================================================================

TEST(UmpToMidiByteStream, CompleteSysExGetsBothDelimiters) {
    UmpToMidiByteStream writer;
    EXPECT_EQ(Convert(writer, {0x3003'4110u, 0x4200'0000u}),
              (std::vector<uint8_t>{0xF0, 0x41, 0x10, 0x42, 0xF7}));
    EXPECT_FALSE(writer.InSysEx());
}

TEST(UmpToMidiByteStream, EmptyCompleteSysExIsJustTheDelimiters) {
    UmpToMidiByteStream writer;
    EXPECT_EQ(Convert(writer, {0x3000'0000u, 0x0000'0000u}),
              (std::vector<uint8_t>{0xF0, 0xF7}));
}

TEST(UmpToMidiByteStream, StartContinueEndProduceOneDelimitedMessage) {
    UmpToMidiByteStream writer;
    const auto bytes = Convert(writer, {
        0x3016'0102u, 0x0304'0506u,   // Start, 6 bytes
        0x3026'0708u, 0x090A'0B0Cu,   // Continue, 6 bytes
        0x3031'0D00u, 0x0000'0000u,   // End, 1 byte
    });
    EXPECT_EQ(bytes, (std::vector<uint8_t>{
        0xF0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0x0A, 0x0B, 0x0C, 0x0D, 0xF7}));
    EXPECT_FALSE(writer.InSysEx());
}

TEST(UmpToMidiByteStream, StartLeavesTheMessageOpen) {
    UmpToMidiByteStream writer;
    EXPECT_EQ(Convert(writer, {0x3011'4100u, 0x0000'0000u}),
              (std::vector<uint8_t>{0xF0, 0x41}));
    EXPECT_TRUE(writer.InSysEx());
}

TEST(UmpToMidiByteStream, ContinueWithNoOpenSysExIsDroppedAndCounted) {
    UmpToMidiByteStream writer;
    // Bare payload would be read under whatever running status the device
    // last saw.
    EXPECT_TRUE(Convert(writer, {0x3021'4100u, 0x0000'0000u}).empty());
    EXPECT_EQ(writer.GetCounters().sysExOutOfSequence, 1u);
}

TEST(UmpToMidiByteStream, EndWithNoOpenSysExIsDroppedAndCounted) {
    UmpToMidiByteStream writer;
    EXPECT_TRUE(Convert(writer, {0x3031'4100u, 0x0000'0000u}).empty());
    EXPECT_EQ(writer.GetCounters().sysExOutOfSequence, 1u);
    EXPECT_EQ(writer.GetCounters().bytesEmitted, 0u);
}

TEST(UmpToMidiByteStream, StartWhileOpenRestartsAndIsCounted) {
    UmpToMidiByteStream writer;
    Convert(writer, {0x3011'4100u, 0x0000'0000u});
    EXPECT_EQ(Convert(writer, {0x3011'4200u, 0x0000'0000u}),
              (std::vector<uint8_t>{0xF0, 0x42}));
    EXPECT_EQ(writer.GetCounters().sysExInterrupted, 1u);
    EXPECT_TRUE(writer.InSysEx());
}

TEST(UmpToMidiByteStream, PayloadLengthAboveSixIsRejected) {
    UmpToMidiByteStream writer;
    // numBytes = 7 cannot fit the two-word packet.
    EXPECT_TRUE(Convert(writer, {0x3007'0102u, 0x0304'0506u}).empty());
    EXPECT_EQ(writer.GetCounters().malformedPackets, 1u);
}

TEST(UmpToMidiByteStream, SysExPayloadByteWithBitSevenSetIsRejected) {
    UmpToMidiByteStream writer;
    EXPECT_TRUE(Convert(writer, {0x3002'4180u, 0x0000'0000u}).empty());
    EXPECT_EQ(writer.GetCounters().malformedPackets, 1u);
    EXPECT_FALSE(writer.InSysEx());
}

TEST(UmpToMidiByteStream, MixedDataSetStatusIsRejected) {
    UmpToMidiByteStream writer;
    // Status 0x8 is a MIDI 2.0 Mixed Data Set header.
    EXPECT_TRUE(Convert(writer, {0x3080'0000u, 0x0000'0000u}).empty());
    EXPECT_EQ(writer.GetCounters().malformedPackets, 1u);
}

//==============================================================================
// Message types this converter does not translate
//==============================================================================

TEST(UmpToMidiByteStream, UtilityPacketsAreIgnoredAndCounted) {
    UmpToMidiByteStream writer;
    EXPECT_TRUE(Convert(writer, {0x0020'1234u}).empty());  // JR Timestamp
    EXPECT_EQ(writer.GetCounters().utilityPacketsIgnored, 1u);
}

TEST(UmpToMidiByteStream, Midi2ChannelVoiceIsSkippedByItsFullLength) {
    UmpToMidiByteStream writer;
    // A two-word MT 0x4 packet followed by a MIDI 1.0 note on. If the skip
    // length were wrong, word1 would be decoded as a message of its own.
    const auto bytes = Convert(writer, {
        0x4090'3C00u, 0xFFFF'FFFFu,
        0x2090'3C40u,
    });
    EXPECT_EQ(bytes, (std::vector<uint8_t>{0x90, 0x3C, 0x40}));
    EXPECT_EQ(writer.GetCounters().unsupportedMessageType, 1u);
}

TEST(UmpToMidiByteStream, FourWordTypesAreSkippedByTheirFullLength) {
    UmpToMidiByteStream writer;
    const auto bytes = Convert(writer, {
        0x5000'0000u, 0xFFFF'FFFFu, 0xFFFF'FFFFu, 0xFFFF'FFFFu,  // Data128
        0xD000'0000u, 0xFFFF'FFFFu, 0xFFFF'FFFFu, 0xFFFF'FFFFu,  // Flex Data
        0xF000'0000u, 0xFFFF'FFFFu, 0xFFFF'FFFFu, 0xFFFF'FFFFu,  // UMP Stream
        0x2090'3C40u,
    });
    EXPECT_EQ(bytes, (std::vector<uint8_t>{0x90, 0x3C, 0x40}));
    EXPECT_EQ(writer.GetCounters().unsupportedMessageType, 3u);
}

TEST(UmpToMidiByteStream, UndefinedThreeWordTypeIsSkippedCorrectly) {
    UmpToMidiByteStream writer;
    // MT 0xB is undefined and three words long.
    const auto bytes = Convert(writer, {
        0xB000'0000u, 0xFFFF'FFFFu, 0xFFFF'FFFFu,
        0x2090'3C40u,
    });
    EXPECT_EQ(bytes, (std::vector<uint8_t>{0x90, 0x3C, 0x40}));
}

//==============================================================================
// Groups
//==============================================================================

TEST(UmpToMidiByteStream, ForeignGroupPacketsAreDropped) {
    UmpToMidiByteStream writer{0};
    EXPECT_TRUE(Convert(writer, {0x2190'3C40u}).empty());
    EXPECT_EQ(writer.GetCounters().foreignGroupPackets, 1u);
}

TEST(UmpToMidiByteStream, ConfiguredGroupIsAccepted) {
    UmpToMidiByteStream writer{0x7};
    EXPECT_EQ(Convert(writer, {0x2790'3C40u}),
              (std::vector<uint8_t>{0x90, 0x3C, 0x40}));
    EXPECT_EQ(writer.GetCounters().foreignGroupPackets, 0u);
}

TEST(UmpToMidiByteStream, GrouplessTypesAreNotFilteredByGroup) {
    UmpToMidiByteStream writer{0x7};
    // A Utility packet has no group field; bits 27..24 must not be read as
    // one, or every utility packet on a nonzero group is misreported.
    EXPECT_TRUE(Convert(writer, {0x0000'0000u}).empty());
    EXPECT_EQ(writer.GetCounters().utilityPacketsIgnored, 1u);
    EXPECT_EQ(writer.GetCounters().foreignGroupPackets, 0u);
}

//==============================================================================
// Truncation and output space
//==============================================================================

TEST(UmpToMidiByteStream, TruncatedMultiWordPacketIsNotConsumed) {
    UmpToMidiByteStream writer;
    const std::vector<UmpWord> words{0x3003'4110u};   // word1 missing
    std::vector<uint8_t> out(32);
    const auto result = writer.Pull(words, out);
    EXPECT_EQ(result.wordsConsumed, 0u);
    EXPECT_EQ(result.bytesWritten, 0u);
    EXPECT_TRUE(result.needsMoreWords);
}

TEST(UmpToMidiByteStream, TruncatedPacketCompletesOnTheNextCall) {
    UmpToMidiByteStream writer;
    std::vector<uint8_t> out(32);

    const std::vector<UmpWord> first{0x2090'3C40u, 0x3003'4110u};
    auto result = writer.Pull(first, out);
    EXPECT_EQ(result.wordsConsumed, 1u);
    EXPECT_TRUE(result.needsMoreWords);
    EXPECT_EQ(result.bytesWritten, 3u);

    const std::vector<UmpWord> rest{0x3003'4110u, 0x4200'0000u};
    result = writer.Pull(rest, out);
    EXPECT_EQ(result.wordsConsumed, 2u);
    EXPECT_FALSE(result.needsMoreWords);
    out.resize(result.bytesWritten);
    EXPECT_EQ(out, (std::vector<uint8_t>{0xF0, 0x41, 0x10, 0x42, 0xF7}));
}

TEST(UmpToMidiByteStream, StopsBeforeAPacketThatCannotFit) {
    UmpToMidiByteStream writer;
    // Fewer than kMaxBytesPerPacket free: nothing is consumed, so no message
    // is ever half-emitted.
    std::vector<uint8_t> out(4);
    const std::vector<UmpWord> words{0x2090'3C40u};
    const auto result = writer.Pull(words, out);
    EXPECT_EQ(result.wordsConsumed, 0u);
    EXPECT_EQ(result.bytesWritten, 0u);
}

//==============================================================================
// Reset
//==============================================================================

TEST(UmpToMidiByteStream, ResetDropsAnOpenSysExWithoutEmittingF7) {
    UmpToMidiByteStream writer;
    Convert(writer, {0x3011'4100u, 0x0000'0000u});
    ASSERT_TRUE(writer.InSysEx());
    const uint64_t before = writer.GetCounters().bytesEmitted;

    writer.Reset();
    EXPECT_FALSE(writer.InSysEx());
    // A trailing 0xF7 would claim a message completed that did not.
    EXPECT_EQ(writer.GetCounters().bytesEmitted, before);

    // After a reset the next Continue has no open message to join.
    EXPECT_TRUE(Convert(writer, {0x3021'4200u, 0x0000'0000u}).empty());
    EXPECT_EQ(writer.GetCounters().sysExOutOfSequence, 1u);
}
