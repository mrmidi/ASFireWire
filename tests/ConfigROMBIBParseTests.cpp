#include <array>
#include <bit>
#include <cstdint>

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

TEST(ConfigROMBIBParseTests, TA1999027_AnnexC_DecodesHeaderAndBusOptions) {
    // TA 1999027 Annex C example (page 25):
    //   q0 = 04 04 EA BF
    //   q1 = 31 33 39 34 ("1394")
    //   q2 = E0 64 61 02 (bus options)
    //   q3/q4 = FF FF FF FF / FF FF FF FF (GUID)
    const std::array<uint32_t, 5> bibWire = {
        WireU32FromBENumeric(0x0404EABFu),
        WireU32FromBENumeric(0x31333934u),
        WireU32FromBENumeric(0xE0646102u),
        WireU32FromBENumeric(0xFFFFFFFFu),
        WireU32FromBENumeric(0xFFFFFFFFu),
    };

    auto bibOpt = ASFW::Discovery::ROMParser::ParseBIB(bibWire.data());
    ASSERT_TRUE(bibOpt.has_value());

    const auto& bib = *bibOpt;

    EXPECT_EQ(bib.busInfoLength, 0x04u);
    EXPECT_EQ(bib.crcLength, 0x04u);
    EXPECT_EQ(bib.crc, 0xEABFu);

    EXPECT_TRUE(bib.irmc);
    EXPECT_TRUE(bib.cmc);
    EXPECT_TRUE(bib.isc);
    EXPECT_FALSE(bib.bmc);
    EXPECT_FALSE(bib.pmc);

    EXPECT_EQ(bib.cycClkAcc, 0x64u);
    EXPECT_EQ(bib.maxRec, 0x6u);
    EXPECT_EQ(bib.maxRom, 0x1u);
    EXPECT_EQ(bib.generation, 0x0u);
    EXPECT_EQ(bib.linkSpd, 0x2u);

    EXPECT_EQ(bib.guid, 0xFFFFFFFFFFFFFFFFULL);
}

