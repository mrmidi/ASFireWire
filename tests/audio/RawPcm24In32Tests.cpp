// RawPcm24In32Tests.cpp
// ASFW - RawPcm24In32 Encoding and Decoding Tests
//

#include <gtest/gtest.h>
#include "Audio/Wire/RawPcm24In32/RawPcm24In32Encoder.hpp"
#include "Audio/Wire/RawPcm24In32/RawPcm24In32Decoder.hpp"
#include <vector>

using namespace ASFW::Encoding;

//==============================================================================
// RawPcm24In32 Encoding Tests
//==============================================================================

TEST(RawPcm24In32Tests, EncodesSilence) {
    uint32_t result = RawPcm24In32::EncodeSilence();
    EXPECT_EQ(result, 0u);
}

TEST(RawPcm24In32Tests, EncodesZeroSample) {
    int32_t sample = 0;
    uint32_t result = RawPcm24In32::Encode(sample);
    EXPECT_EQ(result, 0u);
}

TEST(RawPcm24In32Tests, EncodesPositiveSample) {
    int32_t sample = 0x00123456;
    uint32_t result = RawPcm24In32::Encode(sample);

    // Host order 0x00123456 -> Big endian on wire: 0x56341200 in little-endian representation
    EXPECT_EQ(result, 0x56341200);
}

TEST(RawPcm24In32Tests, EncodesNegativeSample) {
    int32_t sample = static_cast<int32_t>(0x00FEDCBA); // Negative 24-bit value in low bits
    uint32_t result = RawPcm24In32::Encode(sample);

    // Sign-extended: 0xFFFEDCBA
    // Big-endian swapped: 0xBADCFEFF
    EXPECT_EQ(result, 0xBADCFEFF);
}

TEST(RawPcm24In32Tests, EncodesMaxPositive) {
    int32_t sample = 0x007FFFFF;
    uint32_t result = RawPcm24In32::Encode(sample);
    
    // Sign-extended: 0x007FFFFF
    // Big-endian swapped: 0xFFFF7F00
    EXPECT_EQ(result, 0xFFFF7F00);
}

TEST(RawPcm24In32Tests, EncodesMaxNegative) {
    int32_t sample = static_cast<int32_t>(0x00800000);
    uint32_t result = RawPcm24In32::Encode(sample);
    
    // Sign-extended: 0xFF800000
    // Big-endian swapped: 0x000080FF
    EXPECT_EQ(result, 0x000080FF);
}

//==============================================================================
// RawPcm24In32 Decoding Tests
//==============================================================================

TEST(RawPcm24In32Tests, DecodesZeroSample) {
    uint32_t wireQuadlet = 0u;
    auto result = RawPcm24In32::Decode(wireQuadlet);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);
}

TEST(RawPcm24In32Tests, DecodesPositiveSample) {
    // Big-endian wire: 0x00 0x12 0x34 0x56 -> representation in little-endian: 0x56341200
    uint32_t wireQuadlet = 0x56341200;
    auto result = RawPcm24In32::Decode(wireQuadlet);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0x00123456);
}

TEST(RawPcm24In32Tests, DecodesNegativeSample) {
    // Big-endian wire: 0xFF 0xED 0xDC 0xBA -> representation in little-endian: 0xBADCFEFF
    uint32_t wireQuadlet = 0xBADCFEFF;
    auto result = RawPcm24In32::Decode(wireQuadlet);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, static_cast<int32_t>(0xFFFEDCBA));
}

//==============================================================================
// Roundtrip Verification
//==============================================================================

TEST(RawPcm24In32Tests, RoundtripValues) {
    std::vector<int32_t> testSamples = {
        0, 1, -1, 100, -100, 0x123456, -0x123456, 0x7FFFFF, -0x800000
    };

    for (int32_t sample : testSamples) {
        int32_t expected = sample;
        if (expected > 0x7FFFFF) expected = 0x7FFFFF;
        if (expected < -0x800000) expected = -0x800000;
        
        uint32_t encoded = RawPcm24In32::Encode(expected);
        auto decoded = RawPcm24In32::Decode(encoded);
        
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, expected) << "Failed for sample: " << sample;
    }
}
