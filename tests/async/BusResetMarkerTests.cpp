// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// BusResetMarkerTests.cpp — controller-synthesized bus-reset marker decoding.
//
// OHCI delivers a synthetic link-internal ("PHY") packet to the AR Request context on
// every bus reset, carrying the new selfIDGeneration. The field sits in **quadlet 2,
// bits [23:16]** — raw byte offset 10 of the AR packet.
//
// Both reference stacks agree, reached independently:
//   - Linux drivers/firewire/ohci.c:981
//         ohci->request_generation = (p.header[2] >> 16) & 0xff;
//     with p.header[0..2] == buffer[0..2] and header_length == 12 for TCODE_LINK_INTERNAL.
//   - Apple AppleFWOHCI_AsyncReceiveRequest::checkForReceivedPackets, case 0xE
//         *(_DWORD *)(link + 3220) = *((unsigned __int8 *)pkt + 10);
//     after getPacket(0x10) — 3 header quadlets + trailer, same base, same geometry.
//
// An earlier revision read quadlet 1, which reads 0 on real hardware; every logged
// marker showed gen=0. These tests pin the offset so that cannot regress silently.

#include "Async/Rx/ARPacketParser.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using ASFW::Async::ARPacketParser;

// OHCI AR DMA stores each quadlet little-endian in host memory.
void AppendLE32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFU));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFU));
}

// Build a 3-quadlet link-internal AR header (the marker's header_length is 12).
std::vector<uint8_t> MakeMarkerHeader(uint32_t q0, uint32_t q1, uint32_t q2) {
    std::vector<uint8_t> bytes;
    bytes.reserve(12);
    AppendLE32(bytes, q0);
    AppendLE32(bytes, q1);
    AppendLE32(bytes, q2);
    return bytes;
}

constexpr uint32_t kTCodePhyQuadlet = 0xE0000000U; // tcode 0xE in bits [31:28]

// ---- The offset itself --------------------------------------------------------

TEST(BusResetMarker, GenerationComesFromQuadlet2Bits23To16) {
    const auto header = MakeMarkerHeader(kTCodePhyQuadlet, 0x00000000U, 0x00190000U);

    const auto gen = ARPacketParser::ExtractBusResetMarkerGeneration(header);
    ASSERT_TRUE(gen.has_value());
    EXPECT_EQ(*gen, 0x19U) << "generation must come from quadlet 2 bits [23:16] "
                              "(Linux ohci.c:981, AppleFWOHCI case 0xE)";
}

TEST(BusResetMarker, GenerationIsRawByte10OfThePacket) {
    // Apple reads byte 10 directly. Assert the byte-level equivalence so a future
    // endianness change cannot silently drift from the reference.
    auto header = MakeMarkerHeader(kTCodePhyQuadlet, 0xDEADBEEFU, 0x00000000U);
    header[10] = 0x2AU;

    const auto gen = ARPacketParser::ExtractBusResetMarkerGeneration(header);
    ASSERT_TRUE(gen.has_value());
    EXPECT_EQ(*gen, 0x2AU);
}

TEST(BusResetMarker, Quadlet1IsIgnored) {
    // The regression that motivated this: quadlet 1 used to be read instead. If it
    // ever is again, this fails loudly rather than reporting a plausible number.
    const auto header = MakeMarkerHeader(kTCodePhyQuadlet, 0x00FF0000U, 0x00070000U);

    const auto gen = ARPacketParser::ExtractBusResetMarkerGeneration(header);
    ASSERT_TRUE(gen.has_value());
    EXPECT_EQ(*gen, 0x07U) << "quadlet 1 must not contribute to the generation";
    EXPECT_NE(*gen, 0xFFU);
}

TEST(BusResetMarker, Quadlet0IsIgnored) {
    const auto header = MakeMarkerHeader(kTCodePhyQuadlet | 0x00AB0000U, 0U, 0x000C0000U);

    const auto gen = ARPacketParser::ExtractBusResetMarkerGeneration(header);
    ASSERT_TRUE(gen.has_value());
    EXPECT_EQ(*gen, 0x0CU);
}

TEST(BusResetMarker, AdjacentBitsOutsideTheFieldAreMaskedOff) {
    // Bits [31:24] and [15:0] of quadlet 2 must not bleed into the 8-bit field.
    const auto header = MakeMarkerHeader(kTCodePhyQuadlet, 0U, 0xFF11FFFFU);

    const auto gen = ARPacketParser::ExtractBusResetMarkerGeneration(header);
    ASSERT_TRUE(gen.has_value());
    EXPECT_EQ(*gen, 0x11U);
}

// ---- Range ---------------------------------------------------------------------

TEST(BusResetMarker, CoversFullEightBitGenerationRange) {
    for (uint32_t value = 0; value <= 0xFFU; ++value) {
        const auto header = MakeMarkerHeader(kTCodePhyQuadlet, 0U, value << 16);
        const auto gen = ARPacketParser::ExtractBusResetMarkerGeneration(header);
        ASSERT_TRUE(gen.has_value()) << "value=" << value;
        EXPECT_EQ(static_cast<uint32_t>(*gen), value);
    }
}

TEST(BusResetMarker, GenerationZeroIsAValidValueNotAFailure) {
    // Generation 0 is legitimate (it wraps). It must be reported as a value, not
    // conflated with "could not extract" — the old bug made 0 indistinguishable.
    const auto header = MakeMarkerHeader(kTCodePhyQuadlet, 0xFFFFFFFFU, 0x00000000U);

    const auto gen = ARPacketParser::ExtractBusResetMarkerGeneration(header);
    ASSERT_TRUE(gen.has_value());
    EXPECT_EQ(*gen, 0U);
}

// ---- Bounds --------------------------------------------------------------------

TEST(BusResetMarker, ShortHeaderIsRejected) {
    // The guard used to accept 8 bytes, which is one quadlet short of where the
    // generation lives — it would have read past the span.
    for (size_t length = 0; length < 12; ++length) {
        const std::vector<uint8_t> truncated(length, 0xAAU);
        EXPECT_FALSE(ARPacketParser::ExtractBusResetMarkerGeneration(truncated).has_value())
            << "length=" << length << " must be rejected; the field ends at byte 11";
    }
}

TEST(BusResetMarker, ExactlyTwelveBytesIsAccepted) {
    const auto header = MakeMarkerHeader(kTCodePhyQuadlet, 0U, 0x00330000U);
    ASSERT_EQ(header.size(), 12U);

    const auto gen = ARPacketParser::ExtractBusResetMarkerGeneration(header);
    ASSERT_TRUE(gen.has_value());
    EXPECT_EQ(*gen, 0x33U);
}

TEST(BusResetMarker, TrailingBytesBeyondTheHeaderAreIgnored) {
    // A caller may hand over a span that still includes the OHCI trailer.
    auto header = MakeMarkerHeader(kTCodePhyQuadlet, 0U, 0x00440000U);
    AppendLE32(header, 0x94091234U); // trailer: xferStatus/timeStamp
    ASSERT_EQ(header.size(), 16U);

    const auto gen = ARPacketParser::ExtractBusResetMarkerGeneration(header);
    ASSERT_TRUE(gen.has_value());
    EXPECT_EQ(*gen, 0x44U);
}

TEST(BusResetMarker, EmptySpanIsRejected) {
    EXPECT_FALSE(
        ARPacketParser::ExtractBusResetMarkerGeneration(std::span<const uint8_t>{}).has_value());
}

// ---- Consistency with the sibling PHY extractor --------------------------------

TEST(BusResetMarker, SharesQuadletBaseWithPhyPacketExtractor) {
    // ExtractPhyPacketQuadletsHostOrder reads quadlets 1 and 2 as the PHY payload.
    // The marker generation must come from that same quadlet 2 — proving both agree
    // on where header[0] starts, which is what makes the Linux/Apple comparison valid.
    const auto header = MakeMarkerHeader(kTCodePhyQuadlet, 0x11223344U, 0x00550000U);

    const auto phy = ARPacketParser::ExtractPhyPacketQuadletsHostOrder(header);
    ASSERT_TRUE(phy.has_value());
    EXPECT_EQ((*phy)[0], 0x11223344U) << "PHY quadlet 0 is header quadlet 1";
    EXPECT_EQ((*phy)[1], 0x00550000U) << "PHY quadlet 1 is header quadlet 2";

    const auto gen = ARPacketParser::ExtractBusResetMarkerGeneration(header);
    ASSERT_TRUE(gen.has_value());
    EXPECT_EQ(*gen, static_cast<uint8_t>(((*phy)[1] >> 16) & 0xFFU));
}

// ---- Observed-hardware regression ----------------------------------------------

TEST(BusResetMarker, ApogeeDuetUnplugTraceShapeDecodesNonZero) {
    // From the 2026-07-27 Duet hot-unplug trace: 20 markers were logged with gen=0
    // while SelfIDCount advanced 5 -> 25. With the correct offset a marker carrying
    // generation 25 in quadlet 2 decodes as 25, not 0.
    const auto header = MakeMarkerHeader(kTCodePhyQuadlet, 0x00000000U, 0x00190000U);

    const auto gen = ARPacketParser::ExtractBusResetMarkerGeneration(header);
    ASSERT_TRUE(gen.has_value());
    EXPECT_EQ(*gen, 25U);
}

} // namespace
