// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xDotScramblerTests.cpp - the Digi 002/003 playback scrambler.
//
// Golden values cross-validated with Linux sound/firewire/digi00x/amdtp-dot.c
// (dot_scrt, dot_encode_step). During development the implementation was also
// compared against that reference for every key and distance 0..40 and over
// 50M random bytes with zero runs; the reference itself is not in this tree.

#include <gtest/gtest.h>

#include "Audio/Wire/Digi00x/DotScrambler.hpp"

#include <array>
#include <cstdint>

using ASFW::Encoding::Digi00x::DotScrambler;
using ASFW::Encoding::Digi00x::kDotScrambledByte;

namespace {

TEST(Digi00xDotScrambler, SaltValuesMatchTheReference) {
    // Key 0x01: low nibble 1 gives a one-quadlet pattern.
    EXPECT_EQ(DotScrambler::Salt(0x01, 1), 0x6F);
    EXPECT_EQ(DotScrambler::Salt(0x01, 2), 0x00);
    // A key high nibble of 9 bypasses the high-nibble cycle.
    EXPECT_EQ(DotScrambler::Salt(0x98, 1), 0x97);
    // Low nibble 8 has the longest pattern (14), then zero.
    EXPECT_EQ(DotScrambler::Salt(0x18, 14), 0xEF);
    EXPECT_EQ(DotScrambler::Salt(0x18, 15), 0x00);
}

TEST(Digi00xDotScrambler, KeysWithLowNibbleZeroOrFifteenHaveNoPattern) {
    for (const uint8_t high : {0x00, 0x30, 0x90, 0xF0}) {
        EXPECT_EQ(DotScrambler::Salt(static_cast<uint8_t>(high | 0x0), 1), 0x00);
        EXPECT_EQ(DotScrambler::Salt(static_cast<uint8_t>(high | 0xF), 1), 0x00);
    }
}

TEST(Digi00xDotScrambler, GoldenStreamMatchesTheReference) {
    constexpr std::array<uint8_t, 20> in{
        0x12, 0x00, 0x00, 0x00, 0x00, 0x9A, 0x00, 0x00, 0xFF, 0x00,
        0x00, 0x00, 0x37, 0x01, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00};
    constexpr std::array<uint8_t, 20> expected{
        0x12, 0x2E, 0xB1, 0xAF, 0x00, 0x9A, 0x95, 0x9B, 0x6B, 0xF4,
        0x8C, 0x73, 0x6A, 0xF4, 0x8C, 0x73, 0xDD, 0xE2, 0x1E, 0x21};
    DotScrambler scrambler;
    for (size_t i = 0; i < in.size(); ++i) {
        EXPECT_EQ(scrambler.Next(in[i]), expected[i]) << "byte " << i;
    }
}

TEST(Digi00xDotScrambler, TheFirstByteAfterResetPassesThrough) {
    DotScrambler scrambler;
    EXPECT_EQ(scrambler.Next(0x12), 0x12);
    EXPECT_NE(scrambler.Next(0x00), 0x00);  // now inside 0x12's pattern
    scrambler.Reset();
    EXPECT_EQ(scrambler.Next(0x12), 0x12);
}

TEST(Digi00xDotScrambler, LongSilenceStaysSilent) {
    DotScrambler scrambler;
    (void)scrambler.Next(0x18);  // longest pattern: 14 salted quadlets follow
    for (int i = 0; i < 14; ++i) {
        (void)scrambler.Next(0x00);
    }
    for (int i = 0; i < 100'000; ++i) {
        ASSERT_EQ(scrambler.Next(0x00), 0x00) << "quadlet " << i;
    }
}

TEST(Digi00xDotScrambler, OnlyTheMiddleSampleByteOfAQuadletChanges) {
    DotScrambler reference;
    (void)reference.Next(0x01);
    const uint8_t expected = reference.Next(0x55);

    DotScrambler scrambler;
    std::array<uint8_t, 4> first{0x40, 0xAA, 0x01, 0xBB};
    scrambler.ScrambleQuadlet(first);
    std::array<uint8_t, 4> second{0x40, 0xAA, 0x55, 0xBB};
    scrambler.ScrambleQuadlet(second);

    EXPECT_EQ(kDotScrambledByte, 2U);
    EXPECT_EQ(second[0], 0x40);
    EXPECT_EQ(second[1], 0xAA);
    EXPECT_EQ(second[2], expected);
    EXPECT_EQ(second[3], 0xBB);
}

static_assert(DotScrambler::Salt(0x01, 1) == 0x6F, "usable in constant expressions");

} // namespace
