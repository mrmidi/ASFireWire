// BusOptionsFieldsTests.cpp
//
// Unit tests for ASFW::FW::BusOptionsFields, DecodeBusOptions, EncodeBusOptions,
// and SetGeneration in FWCommon.hpp.
//
// Reference: IEEE 1212-2001 §8.3.2 + TA 1999027 Annex C.
// Canonical example bus options quadlet: 0xE0646102
//   Bits [31:29] irmc=1 cmc=1 isc=1    → 0b111 at top
//   Bits [28]    bmc=0
//   Bits [27]    pmc=0
//   Bits [23:16] cyc_clk_acc=0x64 (100 ppm)
//   Bits [15:12] max_rec=6          (512-byte max async payload)
//   Bits [11:10] reserved=0
//   Bits [9:8]   max_ROM=1          (general ROM present)
//   Bits [7:4]   generation=0
//   Bits [3]     reserved=0
//   Bits [2:0]   link_spd=2         (S400)

#include <cstdint>
#include <bit>

#include <gtest/gtest.h>

#include "ASFWDriver/Common/FWCommon.hpp"

namespace {

// TA 1999027 Annex C example bus options quadlet.
constexpr uint32_t kTA1999027BusOptions = 0xE0646102u;

} // namespace

// =============================================================================
// DecodeBusOptions
// =============================================================================

TEST(BusOptionsFieldsTests, Decode_TA1999027_AnnexC) {
    const auto d = ASFW::FW::DecodeBusOptions(kTA1999027BusOptions);

    // Capability flags
    EXPECT_TRUE(d.irmc);
    EXPECT_TRUE(d.cmc);
    EXPECT_TRUE(d.isc);
    EXPECT_FALSE(d.bmc);
    EXPECT_FALSE(d.pmc);

    // Numeric fields
    EXPECT_EQ(d.cycClkAcc, 0x64u);  // 100 ppm
    EXPECT_EQ(d.maxRec,    0x6u);   // 2^(6+1) = 128 bytes min, 512 bytes typical
    EXPECT_EQ(d.maxRom,    0x1u);   // general ROM
    EXPECT_EQ(d.generation, 0x0u);
    EXPECT_EQ(d.linkSpd,   0x2u);   // S400
}

TEST(BusOptionsFieldsTests, Decode_AllZeros_ProducesAllFalseAndZero) {
    const auto d = ASFW::FW::DecodeBusOptions(0u);

    EXPECT_FALSE(d.irmc);
    EXPECT_FALSE(d.cmc);
    EXPECT_FALSE(d.isc);
    EXPECT_FALSE(d.bmc);
    EXPECT_FALSE(d.pmc);
    EXPECT_EQ(d.cycClkAcc,  0u);
    EXPECT_EQ(d.maxRec,     0u);
    EXPECT_EQ(d.maxRom,     0u);
    EXPECT_EQ(d.generation, 0u);
    EXPECT_EQ(d.linkSpd,    0u);
}

// =============================================================================
// EncodeBusOptions / round-trip
// =============================================================================

TEST(BusOptionsFieldsTests, Encode_TA1999027_AnnexC_RoundTrip) {
    // Decode the canonical example, re-encode it, compare with original.
    // Reserved bits [11:10] and [3] are zero in the canonical example, so
    // round-trip must be exact.
    const auto d = ASFW::FW::DecodeBusOptions(kTA1999027BusOptions);
    const uint32_t reEncoded = ASFW::FW::EncodeBusOptions(d);
    EXPECT_EQ(reEncoded, kTA1999027BusOptions);
}

TEST(BusOptionsFieldsTests, Encode_AllTrue_AllMax) {
    ASFW::FW::BusOptionsDecoded d{};
    d.irmc      = true;
    d.cmc       = true;
    d.isc       = true;
    d.bmc       = true;
    d.pmc       = true;
    d.cycClkAcc = 0xFFu;
    d.maxRec    = 0xFu;
    d.maxRom    = 0x3u;
    d.generation = 0xFu;
    d.linkSpd   = 0x7u;

    const uint32_t encoded = ASFW::FW::EncodeBusOptions(d);

    // Verify the fields round-trip cleanly (reserved bits stay 0).
    const auto decoded = ASFW::FW::DecodeBusOptions(encoded);
    EXPECT_TRUE(decoded.irmc);
    EXPECT_TRUE(decoded.cmc);
    EXPECT_TRUE(decoded.isc);
    EXPECT_TRUE(decoded.bmc);
    EXPECT_TRUE(decoded.pmc);
    EXPECT_EQ(decoded.cycClkAcc,  0xFFu);
    EXPECT_EQ(decoded.maxRec,     0xFu);
    EXPECT_EQ(decoded.maxRom,     0x3u);
    EXPECT_EQ(decoded.generation, 0xFu);
    EXPECT_EQ(decoded.linkSpd,    0x7u);
}

// =============================================================================
// SetGeneration
// =============================================================================

TEST(BusOptionsFieldsTests, SetGeneration_UpdatesOnlyGenerationBits) {
    // Start with canonical example (generation=0), bump to generation=9.
    const uint32_t updated = ASFW::FW::SetGeneration(kTA1999027BusOptions, 9);

    const auto d = ASFW::FW::DecodeBusOptions(updated);
    EXPECT_EQ(d.generation, 9u);

    // All other fields must be unchanged.
    EXPECT_TRUE(d.irmc);
    EXPECT_TRUE(d.cmc);
    EXPECT_TRUE(d.isc);
    EXPECT_FALSE(d.bmc);
    EXPECT_FALSE(d.pmc);
    EXPECT_EQ(d.cycClkAcc, 0x64u);
    EXPECT_EQ(d.maxRec,    0x6u);
    EXPECT_EQ(d.maxRom,    0x1u);
    EXPECT_EQ(d.linkSpd,   0x2u);
}

TEST(BusOptionsFieldsTests, SetGeneration_PreservesReservedBits) {
    // Inject non-zero reserved bits [11:10] and [3] into the quadlet to confirm
    // SetGeneration does not corrupt them.
    constexpr uint32_t kWithReserved = kTA1999027BusOptions | 0x00000C08u; // bits 11,10,3
    const uint32_t updated = ASFW::FW::SetGeneration(kWithReserved, 5);

    // Generation updated.
    EXPECT_EQ(ASFW::FW::DecodeBusOptions(updated).generation, 5u);

    // Reserved bits are intact.
    EXPECT_EQ(updated & 0x00000C08u, 0x00000C08u);
}

TEST(BusOptionsFieldsTests, SetGeneration_ClampTo4Bits) {
    // Values > 0xF should be masked to low 4 bits.
    const uint32_t updated = ASFW::FW::SetGeneration(kTA1999027BusOptions, 0x1F); // 5 bits
    EXPECT_EQ(ASFW::FW::DecodeBusOptions(updated).generation, 0xFu); // only low 4 kept
}

// =============================================================================
// Field bit-position regression guards
//
// These catch a regression to the old BIBFields namespace where positions were
// completely wrong (e.g. generation was at bits [27:24] of quadlet 0 instead of
// bits [7:4] of the bus options quadlet 2).
// =============================================================================

TEST(BusOptionsFieldsTests, GenerationField_IsAt_Bits7to4) {
    // Build a quadlet with ONLY generation=1 set, all others zero.
    // Expected: bit 4 set → 0x00000010
    constexpr uint32_t kGeneration1 = 0x00000010u;
    const auto d = ASFW::FW::DecodeBusOptions(kGeneration1);
    EXPECT_EQ(d.generation, 1u);
    EXPECT_EQ(d.linkSpd,    0u);
    EXPECT_EQ(d.maxRec,     0u);
}

TEST(BusOptionsFieldsTests, LinkSpdField_IsAt_Bits2to0) {
    // Build a quadlet with ONLY link_spd=3 (S800) set, all others zero.
    // Expected: bits [2:0] = 3 → 0x00000003
    constexpr uint32_t kS800 = 0x00000003u;
    const auto d = ASFW::FW::DecodeBusOptions(kS800);
    EXPECT_EQ(d.linkSpd,    3u);
    EXPECT_EQ(d.generation, 0u);
    EXPECT_EQ(d.maxRec,     0u);
}

TEST(BusOptionsFieldsTests, MaxRecField_IsAt_Bits15to12) {
    // Build a quadlet with ONLY max_rec=1, all others zero.
    // Expected: bit 12 set → 0x00001000
    constexpr uint32_t kMaxRec1 = 0x00001000u;
    const auto d = ASFW::FW::DecodeBusOptions(kMaxRec1);
    EXPECT_EQ(d.maxRec,  1u);
    EXPECT_EQ(d.maxRom,  0u);
    EXPECT_EQ(d.linkSpd, 0u);
}
