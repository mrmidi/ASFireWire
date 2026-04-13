// AM824EncoderTests.cpp
// ASFW - Phase 1.5 Encoding Tests
//
// Tests for AM824 encoder using real FireBug capture data.
// Reference: 000-48kORIG.txt
//

#include <gtest/gtest.h>
#include "Isoch/Encoding/AM824Encoder.hpp"

using namespace ASFW::Encoding;

//==============================================================================
// Basic Encoding Tests
//==============================================================================

// Silence should be encoded as 0x40000000 (with byte swap)
TEST(AM824EncoderTests, EncodesSilence) {
    uint32_t result = AM824Encoder::encodeSilence();
    
    // After byte swap: 0x40000000 → 0x00000040
    EXPECT_EQ(result, 0x00000040);
}

// Zero sample in 24-in-32 format
TEST(AM824EncoderTests, EncodesZeroSample) {
    int32_t sample = 0x00000000;  // 24-bit zero in lower bits
    uint32_t result = AM824Encoder::encode(sample);
    
    // Same as silence
    EXPECT_EQ(result, 0x00000040);
}

// Positive sample
TEST(AM824EncoderTests, EncodesPositiveSample) {
    // 24-bit sample 0x123456 in lower bits of 32-bit container (0x00XXXXXX format)
    int32_t sample = 0x00123456;
    uint32_t result = AM824Encoder::encode(sample);

    // Before swap: 0x40123456
    // After swap:  0x56341240
    EXPECT_EQ(result, 0x56341240);
}

// Negative sample (two's complement)
TEST(AM824EncoderTests, EncodesNegativeSample) {
    // 24-bit sample 0xFEDCBA (negative in 24-bit two's complement) in lower bits
    int32_t sample = static_cast<int32_t>(0x00FEDCBA);
    uint32_t result = AM824Encoder::encode(sample);

    // Before swap: 0x40FEDCBA
    // After swap:  0xBADCFE40
    EXPECT_EQ(result, 0xBADCFE40);
}

// Maximum positive 24-bit value
TEST(AM824EncoderTests, EncodesMaxPositive) {
    // 0x7FFFFF in lower bits = 0x007FFFFF
    int32_t sample = 0x007FFFFF;
    uint32_t result = AM824Encoder::encode(sample);

    // Before swap: 0x407FFFFF
    // After swap:  0xFFFF7F40
    EXPECT_EQ(result, 0xFFFF7F40);
}

// Maximum negative 24-bit value
TEST(AM824EncoderTests, EncodesMaxNegative) {
    // 0x800000 in lower bits = 0x00800000
    int32_t sample = static_cast<int32_t>(0x00800000);
    uint32_t result = AM824Encoder::encode(sample);

    // Before swap: 0x40800000
    // After swap:  0x00008040
    EXPECT_EQ(result, 0x00008040);
}

//==============================================================================
// FireBug Capture Validation Tests
// Reference: 000-48kORIG.txt cycle 978
//==============================================================================

// Channel 0 sample from capture: 0x40000056
TEST(AM824EncoderTests, MatchesFireBugCapture_QuantizationNoise) {
    // Sample value 0x56 (86 decimal) - quantization noise
    // In 24-in-32 lower-bits format: 0x00000056
    int32_t sample = 0x00000056;
    uint32_t result = AM824Encoder::encode(sample);
    
    // The capture shows 0x40000056 in big-endian wire format
    // Our encoder returns wire format, so compare directly
    // Wire order bytes: 40 00 00 56
    // As little-endian uint32: 0x56000040
    EXPECT_EQ(result, 0x56000040);
}

// Channel 1 sample from capture: 0x40E55654
TEST(AM824EncoderTests, MatchesFireBugCapture_RealAudio) {
    // Sample value 0xE55654 - real audio
    // In 24-in-32 lower-bits format: 0x00E55654
    int32_t sample = static_cast<int32_t>(0x00E55654);
    uint32_t result = AM824Encoder::encode(sample);
    
    // Wire order bytes: 40 E5 56 54
    // As little-endian uint32: 0x5456E540
    EXPECT_EQ(result, 0x5456E540);
}

// Another channel 1 sample: 0x40DBD499
TEST(AM824EncoderTests, MatchesFireBugCapture_RealAudio2) {
    // Sample value 0xDBD499
    // In 24-in-32 lower-bits format: 0x00DBD499
    int32_t sample = static_cast<int32_t>(0x00DBD499);
    uint32_t result = AM824Encoder::encode(sample);
    
    // Wire order bytes: 40 DB D4 99
    // As little-endian uint32: 0x99D4DB40
    EXPECT_EQ(result, 0x99D4DB40);
}

//==============================================================================
// Stereo Frame Encoding Tests
//==============================================================================

TEST(AM824EncoderTests, EncodesStereoFrame) {
    int32_t left = 0x00001234;
    int32_t right = 0x00005678;
    uint32_t out[2];
    
    AM824Encoder::encodeStereoFrame(left, right, out);
    
    // Verify both samples are encoded correctly
    EXPECT_EQ(out[0], AM824Encoder::encode(left));
    EXPECT_EQ(out[1], AM824Encoder::encode(right));
}

TEST(AM824EncoderTests, EncodesStereoFrameSilence) {
    int32_t left = 0;
    int32_t right = 0;
    uint32_t out[2];
    
    AM824Encoder::encodeStereoFrame(left, right, out);
    
    EXPECT_EQ(out[0], AM824Encoder::encodeSilence());
    EXPECT_EQ(out[1], AM824Encoder::encodeSilence());
}

//==============================================================================
// Label Byte Verification
//==============================================================================

TEST(AM824EncoderTests, UsesCorrectLabel) {
    EXPECT_EQ(kAM824LabelMBLA, 0x40);
}

// Verify the label appears in the correct byte position (MSB in host order)
TEST(AM824EncoderTests, LabelInCorrectPosition) {
    int32_t sample = 0x00000000;
    uint32_t result = AM824Encoder::encode(sample);
    
    // After byte swap for wire order, label 0x40 should be in LSB
    // (because it was in MSB before swap)
    EXPECT_EQ(result & 0x000000FF, 0x40);
}

//==============================================================================
// Constexpr Verification (compile-time evaluation)
//==============================================================================

TEST(AM824EncoderTests, IsConstexpr) {
    // These should compile if encode() is truly constexpr
    constexpr uint32_t silence = AM824Encoder::encodeSilence();
    constexpr uint32_t sample = AM824Encoder::encode(0x00123456);
    
    EXPECT_EQ(silence, 0x00000040);
    EXPECT_EQ(sample, 0x56341240);
}
