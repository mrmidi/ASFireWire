#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ASFWDriver/ConfigROM/ConfigROMStore.hpp"

namespace {

constexpr uint32_t WireU32FromBENumeric(uint32_t be) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return __builtin_bswap32(be);
    }
    return be;
}

} // namespace

TEST(TextDescriptorLeafParseTests, TAExampleLeaf_ParsesVendorName) {
    // IEEE 1212-2001 Figure 28 textual descriptor leaf layout:
    //   +0: [leaf_length:16][crc:16]
    //   +1: [descriptor_type:8][specifier_ID:24]  (0 for minimal ASCII)
    //   +2: [width:8][character_set:8][language:16] (0 for minimal ASCII)
    //   +3..: ASCII text quadlets
    //
    // TA 1999027 Annex C (page 25) example vendor name: "Vendor Name"
    //
    // leaf_length=5 => quadlets after header: typeSpec + width + 3 text quadlets
    const std::array<uint32_t, 6> leafWire = {
        WireU32FromBENumeric(0x00050000u),  // header: leaf_length=5, crc ignored by parser
        WireU32FromBENumeric(0x00000000u),  // type/specifier
        WireU32FromBENumeric(0x00000000u),  // width/charset/lang (minimal ASCII)
        WireU32FromBENumeric(0x56656E64u),  // "Vend"
        WireU32FromBENumeric(0x6F72204Eu),  // "or N"
        WireU32FromBENumeric(0x616D6500u),  // "ame\\0"
    };

    const std::string parsed = ASFW::Discovery::ROMParser::ParseTextDescriptorLeaf(
        leafWire.data(),
        static_cast<uint32_t>(leafWire.size()),
        /*leafOffsetQuadlets=*/0,
        "big");

    EXPECT_EQ(parsed, "Vendor Name");
}

TEST(TextDescriptorLeafParseTests, TypeSpecMustBeAtPlus1_NotPlus2) {
    // If the parser incorrectly reads type/specifier from +2, it would treat this as valid
    // (since +2 is 0), and return the text. Correct behavior is to reject due to non-zero +1.
    const std::array<uint32_t, 6> leafWire = {
        WireU32FromBENumeric(0x00050000u),  // header: leaf_length=5
        WireU32FromBENumeric(0x01000000u),  // type/specifier: descriptor_type=1 (invalid for text)
        WireU32FromBENumeric(0x00000000u),  // width/charset/lang (minimal ASCII)
        WireU32FromBENumeric(0x56656E64u),  // "Vend"
        WireU32FromBENumeric(0x6F72204Eu),  // "or N"
        WireU32FromBENumeric(0x616D6500u),  // "ame\\0"
    };

    const std::string parsed = ASFW::Discovery::ROMParser::ParseTextDescriptorLeaf(
        leafWire.data(),
        static_cast<uint32_t>(leafWire.size()),
        /*leafOffsetQuadlets=*/0,
        "big");

    EXPECT_TRUE(parsed.empty());
}

