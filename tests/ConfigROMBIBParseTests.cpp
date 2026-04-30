#include <array>
#include <bit>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "ASFWDriver/ConfigROM/ConfigROMParser.hpp"

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

    auto bibRes = ASFW::Discovery::ConfigROMParser::ParseBIB(std::span{bibWire});
    ASSERT_TRUE(bibRes.has_value());

    const auto& bib = bibRes->bib;

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

TEST(ConfigROMBIBParseTests, CRC_Mismatch_IsWarning_NotFailure) {
    const std::array<uint32_t, 5> bibWire = {
        WireU32FromBENumeric(0x0404EABEu), // wrong CRC on purpose (expected 0xEABF)
        WireU32FromBENumeric(0x31333934u),
        WireU32FromBENumeric(0xE0646102u),
        WireU32FromBENumeric(0xFFFFFFFFu),
        WireU32FromBENumeric(0xFFFFFFFFu),
    };

    auto bibRes = ASFW::Discovery::ConfigROMParser::ParseBIB(std::span{bibWire});
    ASSERT_TRUE(bibRes.has_value());
    EXPECT_EQ(bibRes->crcStatus, ASFW::Discovery::ConfigROMParser::CRCStatus::Mismatch);
    ASSERT_TRUE(bibRes->computed.has_value());
    EXPECT_EQ(bibRes->computed.value(), 0xEABFu);
    EXPECT_EQ(bibRes->bib.crc, 0xEABEu);
}

TEST(ConfigROMParserTests, SBP2ManagementAgentOffsetUsesCombinedCSRKey54) {
    const std::array<uint32_t, 5> unitDirectoryWire = {
        WireU32FromBENumeric(0x00040000u),
        WireU32FromBENumeric(0x1200609Eu),
        WireU32FromBENumeric(0x13010483u),
        WireU32FromBENumeric(0x5400C000u),
        WireU32FromBENumeric(0x14060000u),
    };

    auto entries = ASFW::Discovery::ConfigROMParser::ParseRootDirectory(
        std::span{unitDirectoryWire},
        static_cast<uint32_t>(unitDirectoryWire.size()));
    ASSERT_TRUE(entries.has_value());

    bool foundManagementAgent = false;
    bool foundLun = false;
    for (const auto& entry : *entries) {
        if (entry.key == ASFW::Discovery::CfgKey::Management_Agent_Offset) {
            foundManagementAgent = true;
            EXPECT_EQ(entry.value, 0x00C000u);
            EXPECT_EQ(entry.entryType, 1u);
        }
        if (entry.key == ASFW::Discovery::CfgKey::Logical_Unit_Number) {
            foundLun = true;
            EXPECT_EQ(entry.value, 0x060000u);
            EXPECT_EQ(entry.entryType, 0u);
        }
    }

    EXPECT_TRUE(foundManagementAgent);
    EXPECT_TRUE(foundLun);
}
