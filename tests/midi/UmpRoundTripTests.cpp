// UmpRoundTripTests.cpp
// ASFW - WP-3 round-trip properties
//
// These run the two converters against each other. They are a consistency
// check, not a substitute for the independent vectors in the other two files:
// a shared misunderstanding of a layout would round-trip perfectly.

#include <gtest/gtest.h>

#include <vector>

#include "Midi/Ump/MidiByteStreamToUmp.hpp"
#include "Midi/Ump/UmpToMidiByteStream.hpp"

using namespace ASFW::Midi::Ump;

namespace {

/// bytes -> UMP -> bytes.
std::vector<uint8_t> RoundTrip(std::vector<uint8_t> bytes, uint8_t group = 0) {
    MidiByteStreamToUmp parser{group};
    std::vector<UmpWord> words(
        bytes.size() * MidiByteStreamToUmp::kMaxWordsPerByte + 8);
    const auto push = parser.Push(bytes, words);
    EXPECT_EQ(push.bytesConsumed, bytes.size());
    words.resize(push.wordsWritten);

    UmpToMidiByteStream writer{group};
    std::vector<uint8_t> out(
        words.size() * UmpToMidiByteStream::kMaxBytesPerPacket + 8);
    const auto pull = writer.Pull(words, out);
    EXPECT_EQ(pull.wordsConsumed, words.size());
    out.resize(pull.bytesWritten);
    return out;
}

} // namespace

TEST(UmpRoundTrip, ChannelVoiceMessagesSurvive) {
    const std::vector<uint8_t> stream{
        0x92, 0x3C, 0x40,   // note on
        0x82, 0x3C, 0x00,   // note off
        0xB0, 0x07, 0x64,   // control change
        0xC5, 0x2A,         // program change
        0xD0, 0x55,         // channel pressure
        0xE3, 0x00, 0x40,   // pitch bend
        0xA1, 0x3C, 0x10,   // poly pressure
    };
    EXPECT_EQ(RoundTrip(stream), stream);
}

TEST(UmpRoundTrip, RunningStatusIsExpandedNotPreserved) {
    // Input uses running status; output must repeat the status byte. This is
    // a deliberate asymmetry, so it is asserted rather than round-tripped.
    const std::vector<uint8_t> in{0x90, 0x3C, 0x40, 0x3E, 0x50};
    const std::vector<uint8_t> expected{0x90, 0x3C, 0x40, 0x90, 0x3E, 0x50};
    EXPECT_EQ(RoundTrip(in), expected);
}

TEST(UmpRoundTrip, SystemCommonAndRealTimeSurvive) {
    const std::vector<uint8_t> stream{
        0xF1, 0x21,         // quarter frame
        0xF2, 0x12, 0x34,   // song position
        0xF3, 0x07,         // song select
        0xF6,               // tune request
        0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF,
    };
    EXPECT_EQ(RoundTrip(stream), stream);
}

TEST(UmpRoundTrip, SysExOfEveryLengthAcrossThePacketBoundarySurvives) {
    for (std::size_t n = 0; n <= 20; ++n) {
        std::vector<uint8_t> stream{0xF0};
        for (std::size_t i = 0; i < n; ++i)
            stream.push_back(static_cast<uint8_t>(i & 0x7F));
        stream.push_back(0xF7);
        EXPECT_EQ(RoundTrip(stream), stream) << "payload bytes: " << n;
    }
}

TEST(UmpRoundTrip, RealTimeInterleavedInSysExIsHoistedAheadOfIt) {
    // The real-time byte is emitted the moment it arrives, so it leads the
    // SysEx packet it interrupted rather than staying in place. Both orders
    // are valid MIDI; assert the actual one so a change is visible.
    const std::vector<uint8_t> in{0xF0, 0x41, 0xF8, 0x10, 0xF7};
    const std::vector<uint8_t> expected{0xF8, 0xF0, 0x41, 0x10, 0xF7};
    EXPECT_EQ(RoundTrip(in), expected);
}

TEST(UmpRoundTrip, RealTimeInterleavedInAChannelMessageIsHoisted) {
    const std::vector<uint8_t> in{0x90, 0x3C, 0xF8, 0x40};
    const std::vector<uint8_t> expected{0xF8, 0x90, 0x3C, 0x40};
    EXPECT_EQ(RoundTrip(in), expected);
}

TEST(UmpRoundTrip, MixedTrafficSurvives) {
    const std::vector<uint8_t> in{
        0x90, 0x3C, 0x40,
        0xF0, 0x41, 0x10, 0x42, 0x11, 0x01, 0x00, 0xF7,   // 7-byte SysEx
        0xF8,
        0xB0, 0x07, 0x64,
        0xF0, 0xF7,                                        // empty SysEx
        0x80, 0x3C, 0x00,
    };
    EXPECT_EQ(RoundTrip(in), in);
}

TEST(UmpRoundTrip, NonZeroGroupSurvives) {
    const std::vector<uint8_t> stream{0x92, 0x3C, 0x40, 0xF0, 1, 2, 3, 0xF7};
    EXPECT_EQ(RoundTrip(stream, 0x9), stream);
}

TEST(UmpRoundTrip, EveryChannelAndStatusCombinationSurvives) {
    for (uint8_t high = 0x80; high < 0xF0; high = static_cast<uint8_t>(high + 0x10)) {
        for (uint8_t channel = 0; channel < 16; ++channel) {
            const auto status = static_cast<uint8_t>(high | channel);
            std::vector<uint8_t> stream{status, 0x3C};
            if (ChannelMessageDataBytes(status) == 2) stream.push_back(0x40);
            EXPECT_EQ(RoundTrip(stream), stream)
                << "status " << std::hex << int(status);
        }
    }
}

TEST(UmpRoundTrip, FullSevenBitDataRangeSurvives) {
    for (uint16_t value = 0; value < 128; ++value) {
        const auto v = static_cast<uint8_t>(value);
        const std::vector<uint8_t> stream{0x90, v, v};
        EXPECT_EQ(RoundTrip(stream), stream) << "value " << int(v);
    }
}
