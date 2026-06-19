//
// StreamFormatParserTests.cpp
// ASFW Tests
//
// Tests for StreamFormatParser using real Apogee Duet response data
// Reference: FWA/discovery.txt captures from actual device
//

#include <gtest/gtest.h>
#include "Protocols/AVC/StreamFormats/StreamFormatParser.hpp"
#include "Protocols/AVC/StreamFormats/StreamFormatTypes.hpp"

using namespace ASFW::Protocols::AVC::StreamFormats;

//==============================================================================
// Compound AM824 Format Tests (0x90 0x40)
//==============================================================================

// Real data from FWA discovery.txt line 138:
// RSP: 0x0C 0xFF 0xBF 0xC0 0x00 0x00 0x00 0x00 0xFF 0x01 0x90 0x40 0x03 0x02 0x01 0x02 0x06
// Format block starts at byte 10: 0x90 0x40 0x03 0x02 0x01 0x02 0x06
// Structure: [0x90=AM824] [0x40=compound] [0x03=44.1kHz] [0x02=sync] [0x01=numFields] [0x02 0x06=2ch MBLA]
TEST(StreamFormatParserTests, ParsesCompoundAM824_441kHz_2ch) {
    // Compound AM824, 44.1kHz, 1 format field with 2ch MBLA
    // Rate code 0x03 = 44.1kHz per IEC 61883-6
    uint8_t data[] = { 0x90, 0x40, 0x03, 0x02, 0x01, 0x02, 0x06 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->formatHierarchy, FormatHierarchy::kCompoundAM824);
    EXPECT_EQ(result->subtype, AM824Subtype::kCompound);
    EXPECT_EQ(result->sampleRate, SampleRate::k44100Hz);
    // FIX: totalChannels is the SUM of channel counts from all format fields
    // byte[4]=0x01 means 1 format field, that field says 2 channels of MBLA
    EXPECT_EQ(result->totalChannels, 2);
    ASSERT_EQ(result->channelFormats.size(), 1);
    EXPECT_EQ(result->channelFormats[0].channelCount, 2);
    EXPECT_EQ(result->channelFormats[0].formatCode, StreamFormatCode::kMBLA);
}

// From discovery.txt line 168: 48kHz format
// Format: 0x90 0x40 0x04 0x02 0x01 0x02 0x06
// Structure: [0x90=AM824] [0x40=compound] [0x04=48kHz] [0x02=sync] [0x01=numFields] [0x02 0x06=2ch MBLA]
TEST(StreamFormatParserTests, ParsesCompoundAM824_48kHz_2ch) {
    // Compound AM824, 48kHz, 1 format field with 2ch MBLA
    // Rate code 0x04 = 48kHz
    uint8_t data[] = { 0x90, 0x40, 0x04, 0x02, 0x01, 0x02, 0x06 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::k48000Hz);
    // FIX: totalChannels = sum of format field channel counts = 2
    EXPECT_EQ(result->totalChannels, 2);
}

// From discovery.txt line 182: 88.2kHz format
// Format: 0x90 0x40 0x0A 0x02 0x01 0x02 0x06
TEST(StreamFormatParserTests, ParsesCompoundAM824_882kHz_2ch) {
    // Compound AM824, 88.2kHz, 2ch MBLA
    // Rate code 0x0A = 88.2kHz
    uint8_t data[] = { 0x90, 0x40, 0x0A, 0x02, 0x01, 0x02, 0x06 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::k88200Hz);
}

// From discovery.txt line 196: 96kHz format
// Format: 0x90 0x40 0x05 0x02 0x01 0x02 0x06
TEST(StreamFormatParserTests, ParsesCompoundAM824_96kHz_2ch) {
    // Compound AM824, 96kHz, 2ch MBLA
    // Rate code 0x05 = 96kHz
    uint8_t data[] = { 0x90, 0x40, 0x05, 0x02, 0x01, 0x02, 0x06 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::k96000Hz);
}

//==============================================================================
// Simple AM824 Format Tests (0x90 0x00)
//==============================================================================

// From discovery.txt line 465: Simple 3-byte format (Sync stream)
// RSP: 0x0C 0x60 0xBF 0xC0 0x00 0x01 0x02 0xFF 0xFF 0x01 0x90 0x00 0x40
// Format block: 0x90 0x00 0x40 (3 bytes)
TEST(StreamFormatParserTests, ParsesSimpleAM824_3Byte_SyncStream) {
    // Simple AM824, 3-byte, sync stream indicator
    uint8_t data[] = { 0x90, 0x00, 0x40 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->formatHierarchy, FormatHierarchy::kAM824);
    EXPECT_EQ(result->subtype, AM824Subtype::kSimple);
    EXPECT_EQ(result->sampleRate, SampleRate::kDontCare);
    EXPECT_EQ(result->totalChannels, 2); // Simple format defaults to stereo
}

// 6-byte simple format with rate in nibble at byte[4]
// Note: The rate extraction has fallback order: byte[2] nibble -> byte[5] MusicSubunit code -> byte[4] nibble
TEST(StreamFormatParserTests, ParsesSimpleAM824_6Byte_48kHz) {
    // Simple AM824, 6-byte, rate nibble in byte[2] = 0x40 -> 48kHz
    // (byte[5]=0x00 would map to 32kHz via MusicSubunit table, so we use byte[2] for priority)
    uint8_t data[] = { 0x90, 0x00, 0x40, 0x00, 0x00, 0x00 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->formatHierarchy, FormatHierarchy::kAM824);
    EXPECT_EQ(result->subtype, AM824Subtype::kSimple);
    EXPECT_EQ(result->sampleRate, SampleRate::k48000Hz);
}

// Apogee/OXFW quirk: rate encoded in byte2 nibble (0x40) should map to 48 kHz
TEST(StreamFormatParserTests, ParsesApogeeNibbleRate48k) {
    uint8_t data[] = { 0x90, 0x00, 0x40, 0x03, 0x02, 0x01 };
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::k48000Hz);
}

// Apogee/OXFW quirk: when nibble is 0x00, use music sample rate code in byte5
TEST(StreamFormatParserTests, ParsesApogeeMusicRate441) {
    uint8_t data[] = { 0x90, 0x00, 0x00, 0x40, 0x02, 0x01 };
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::k44100Hz);
}

//==============================================================================
// Validation Tests
//==============================================================================

// Standard AM824 (0x90) format - the only valid format hierarchy now
TEST(StreamFormatParserTests, ParsesStandardAM824) {
    uint8_t data[] = { 0x90, 0x00, 0x00, 0x00, 0x40, 0x00 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->formatHierarchy, FormatHierarchy::kAM824);
}

// Reject invalid format hierarchy 0xFF
TEST(StreamFormatParserTests, RejectsInvalidFormatHierarchy) {
    uint8_t data[] = { 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    EXPECT_FALSE(result.has_value());
}

// Reject legacy format 0x00 (no longer accepted to prevent garbage parsing)
TEST(StreamFormatParserTests, RejectsLegacySimple0x00) {
    // Was previously accepted but caused garbage parsing when offset was wrong
    uint8_t data[] = { 0x00, 0x00, 0x00, 0x00, 0x30, 0x00 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    EXPECT_FALSE(result.has_value());
}

// Reject legacy format 0x01 (no longer accepted)
TEST(StreamFormatParserTests, RejectsLegacyGeneric0x01) {
    uint8_t data[] = { 0x01, 0x00, 0x00, 0x00, 0x40, 0x00 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    EXPECT_FALSE(result.has_value());
}

// Reject data that's too short
TEST(StreamFormatParserTests, RejectsTooShortData) {
    uint8_t data[] = { 0x90 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    EXPECT_FALSE(result.has_value());
}

// Reject null pointer
TEST(StreamFormatParserTests, RejectsNullPointer) {
    auto result = StreamFormatParser::Parse(nullptr, 6);
    
    EXPECT_FALSE(result.has_value());
}

// Reject zero length
TEST(StreamFormatParserTests, RejectsZeroLength) {
    uint8_t data[] = { 0x90, 0x40 };
    
    auto result = StreamFormatParser::Parse(data, 0);
    
    EXPECT_FALSE(result.has_value());
}

// Reject unknown subtype
TEST(StreamFormatParserTests, RejectsUnknownSubtype) {
    // 0xFF is not a valid subtype
    uint8_t data[] = { 0x90, 0xFF, 0x00, 0x00, 0x00, 0x00 };
    
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    
    EXPECT_FALSE(result.has_value());
}

//==============================================================================
// Sample Rate Coverage Tests
//==============================================================================

TEST(StreamFormatParserTests, ParsesSampleRate22050Hz) {
    uint8_t data[] = { 0x90, 0x40, 0x00, 0x00, 0x02, 0x02, 0x06 }; // Rate 0x00
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::k22050Hz);
}

TEST(StreamFormatParserTests, ParsesSampleRate24000Hz) {
    uint8_t data[] = { 0x90, 0x40, 0x01, 0x00, 0x02, 0x02, 0x06 }; // Rate 0x01
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::k24000Hz);
}

TEST(StreamFormatParserTests, ParsesSampleRate32000Hz) {
    uint8_t data[] = { 0x90, 0x40, 0x02, 0x00, 0x02, 0x02, 0x06 }; // Rate 0x02
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::k32000Hz);
}

TEST(StreamFormatParserTests, ParsesSampleRate176400Hz) {
    uint8_t data[] = { 0x90, 0x40, 0x06, 0x00, 0x02, 0x02, 0x06 }; // Rate 0x06
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::k176400Hz);
}

TEST(StreamFormatParserTests, ParsesSampleRate192000Hz) {
    uint8_t data[] = { 0x90, 0x40, 0x07, 0x00, 0x02, 0x02, 0x06 }; // Rate 0x07
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::k192000Hz);
}

TEST(StreamFormatParserTests, ParsesSampleRateDontCare) {
    uint8_t data[] = { 0x90, 0x40, 0x0F, 0x00, 0x02, 0x02, 0x06 }; // Rate 0x0F
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::kDontCare);
}

TEST(StreamFormatParserTests, ParsesUnknownSampleRate) {
    uint8_t data[] = { 0x90, 0x40, 0x0E, 0x00, 0x02, 0x02, 0x06 }; // Rate 0x0E (undefined)
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->sampleRate, SampleRate::kUnknown);
}

//==============================================================================
// Sync Mode Tests
//==============================================================================

TEST(StreamFormatParserTests, ParsesSyncModeEnabled) {
    // Byte[3] bit 2 set (0x04) = synchronized
    uint8_t data[] = { 0x90, 0x40, 0x03, 0x04, 0x02, 0x02, 0x06 };
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->syncMode, SyncMode::kSynchronized);
}

TEST(StreamFormatParserTests, ParsesSyncModeDisabled) {
    // Byte[3] bit 2 clear = no sync
    uint8_t data[] = { 0x90, 0x40, 0x03, 0x00, 0x02, 0x02, 0x06 };
    auto result = StreamFormatParser::Parse(data, sizeof(data));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->syncMode, SyncMode::kNoSync);
}