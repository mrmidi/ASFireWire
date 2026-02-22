// TxVerifierDecodeTests.cpp
// ASFW - Host-safe unit tests for dev-only IT TX verifier decode logic

#include <gtest/gtest.h>

#include "../ASFWDriver/Isoch/Transmit/TxVerifierDecode.hpp"
#include "../ASFWDriver/Isoch/Encoding/CIPHeaderBuilder.hpp"
#include "../ASFWDriver/Isoch/Encoding/AM824Encoder.hpp"

using ASFW::Isoch::TxVerify::ByteSwap32;
using ASFW::Isoch::TxVerify::ParseCIPFromHostWords;
using ASFW::Isoch::TxVerify::DbcContinuity;

TEST(TxVerifierDecode, CIPParseMatchesBuilderWireFields) {
    ASFW::Encoding::CIPHeaderBuilder builder(/*sid=*/0, /*dbs=*/2);

    constexpr uint8_t dbc = 0xA8;
    constexpr uint16_t syt = 0x5350;
    const auto h = builder.build(dbc, syt, /*isNoData=*/false);

    // Sanity: what FireBug prints (wire order) is the byteswapped view of host words.
    EXPECT_EQ(ByteSwap32(h.q0), 0x000200A8u);
    EXPECT_EQ(ByteSwap32(h.q1), 0x90025350u);

    const auto p = ParseCIPFromHostWords(h.q0, h.q1);
    EXPECT_EQ(p.eoh0, 0);
    EXPECT_EQ(p.sid, 0);
    EXPECT_EQ(p.dbs, 2);
    EXPECT_EQ(p.dbc, dbc);
    EXPECT_EQ(p.eoh1, 2);
    EXPECT_EQ(p.fmt, ASFW::Encoding::kCIPFormatAM824);
    EXPECT_EQ(p.fdf, ASFW::Encoding::kSFC_48kHz);
    EXPECT_EQ(p.syt, syt);
}

TEST(TxVerifierDecode, AM824LabelExtraction) {
    const uint32_t silence = ASFW::Encoding::AM824Encoder::encodeSilence();
    EXPECT_TRUE(ASFW::Isoch::TxVerify::HasValidAM824Label(silence, ASFW::Encoding::kAM824LabelMBLA));
    EXPECT_EQ(ASFW::Isoch::TxVerify::AM824LabelByte(silence), ASFW::Encoding::kAM824LabelMBLA);

    EXPECT_FALSE(ASFW::Isoch::TxVerify::HasValidAM824Label(0x00000000u, ASFW::Encoding::kAM824LabelMBLA));
    EXPECT_EQ(ASFW::Isoch::TxVerify::AM824LabelByte(0x00000000u), 0);
}

TEST(TxVerifierDecode, DbcContinuityIgnoresNoDataAndDetectsDiscontinuity) {
    DbcContinuity chk(/*blocksPerDataPacket=*/8);

    // Before first DATA, NO-DATA should not seed continuity state.
    EXPECT_TRUE(chk.Observe(/*isDataPacket=*/false, /*dbc=*/0xB0));
    EXPECT_FALSE(chk.HasLastData());

    // First DATA seeds last DBC.
    EXPECT_TRUE(chk.Observe(/*isDataPacket=*/true, /*dbc=*/0xA8));
    EXPECT_TRUE(chk.HasLastData());
    EXPECT_EQ(chk.LastDataDbc(), 0xA8);

    // NO-DATA carries the *next* DATA DBC in blocking cadence; verifier ignores it.
    EXPECT_TRUE(chk.Observe(/*isDataPacket=*/false, /*dbc=*/0xB0));
    EXPECT_EQ(chk.LastDataDbc(), 0xA8);

    // Next DATA must match expected +8.
    EXPECT_TRUE(chk.Observe(/*isDataPacket=*/true, /*dbc=*/0xB0));
    EXPECT_EQ(chk.LastDataDbc(), 0xB0);

    // Discontinuity.
    EXPECT_FALSE(chk.Observe(/*isDataPacket=*/true, /*dbc=*/0xC0));
}

TEST(TxVerifierDecode, DbcContinuityWrapsMod256) {
    DbcContinuity chk(/*blocksPerDataPacket=*/8);
    EXPECT_TRUE(chk.Observe(true, 0xF8));
    EXPECT_TRUE(chk.Observe(true, 0x00)); // 0xF8 + 0x08 = 0x00
}

