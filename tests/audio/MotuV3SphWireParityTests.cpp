// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Pins the MOTU V3 SPH to the shape the device's own stack puts on the wire.
//
// The golden values below are raw first-block SPH quadlets read out of a
// passive bus capture of the official driver on OS X 10.11 driving an 828 Mk3,
// so they are observed wire behaviour, not inference. Both directions agree,
// across every data packet in the capture, that bits 31:25 (the OHCI cycle
// timer's `seconds` field) are zero. An earlier revision filled that field from
// the host clock; this file exists so that regression cannot come back silently.

#include "Audio/DriverKit/Runtime/MotuPhaseTrace.hpp"
#include "Audio/Wire/AMDTP/MotuV3WireFormat.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

namespace MotuV3Wire = ASFW::Protocols::Audio::AMDTP::MotuV3Wire;

constexpr int64_t kTicksPerSecond =
    static_cast<int64_t>(MotuV3Wire::kTicksPerSecond);

// Bits 31:25 of an OHCI cycle timer. The SPH must never reach them.
constexpr uint32_t kSecondsFieldShift = 25;
constexpr uint32_t kFirstTickAboveTheSphField = 0x02000000u;

// Every twelfth data packet of each direction, plus that direction's maximum.
constexpr std::array<uint32_t, 12> kCapturedHostToDeviceSph = {
    0x01072000, 0x011ddbff, 0x01349bff, 0x014b5bff,
    0x0161e125, 0x0177a50d, 0x018e2235, 0x01a5d177,
    0x01bd0775, 0x01d3c785, 0x01ea8795, 0x01f3f39c,
};

constexpr std::array<uint32_t, 12> kCapturedDeviceToHostSph = {
    0x0104a4de, 0x011be4ef, 0x013324ff, 0x014a6510,
    0x0161a520, 0x0178e530, 0x01902541, 0x01a76551,
    0x01bea561, 0x01d5e572, 0x01ed2582, 0x01f3f987,
};

TEST(MotuV3SphWireParityTests, CapturedOriginalStackNeverFillsTheSecondsField) {
    for (const uint32_t sph : kCapturedHostToDeviceSph) {
        SCOPED_TRACE(sph);
        EXPECT_EQ(sph >> kSecondsFieldShift, 0u);
        EXPECT_LT(sph, kFirstTickAboveTheSphField);
    }
    for (const uint32_t sph : kCapturedDeviceToHostSph) {
        SCOPED_TRACE(sph);
        EXPECT_EQ(sph >> kSecondsFieldShift, 0u);
        EXPECT_LT(sph, kFirstTickAboveTheSphField);
    }
}

// The parity statement proper: fed the tick position the captured value stands
// for, our encoder reproduces that captured quadlet bit for bit. This is what
// "we agree with the oracle on this field" means operationally -- checking only
// that our own output has a zero seconds field would pass for an encoder that
// got the cycle/offset split wrong as well.
TEST(MotuV3SphWireParityTests, EncoderReproducesCapturedWireValuesExactly) {
    const auto checkRoundTrip = [](uint32_t captured) {
        SCOPED_TRACE(captured);
        const int64_t ticks = ASFW::Timing::encodedTstampToOffsets(captured);
        EXPECT_LT(ticks, kTicksPerSecond);
        EXPECT_EQ(MotuV3Wire::EncodeSph(ticks), captured);
    };
    for (const uint32_t sph : kCapturedHostToDeviceSph) {
        checkRoundTrip(sph);
    }
    for (const uint32_t sph : kCapturedDeviceToHostSph) {
        checkRoundTrip(sph);
    }
}

TEST(MotuV3SphWireParityTests, EncodeSphStaysInsideTheSphFieldAcrossTheDomain) {
    constexpr std::array<int64_t, 9> kProbes = {
        0,
        1,
        MotuV3Wire::kTicksPerCycle - 1,
        MotuV3Wire::kTicksPerCycle,
        kTicksPerSecond / 2,
        kTicksPerSecond - 1,
        kTicksPerSecond,
        kTicksPerSecond * 7 + 4096,
        -1,
    };

    for (const int64_t ticks : kProbes) {
        SCOPED_TRACE(ticks);
        const uint32_t sph = MotuV3Wire::EncodeSph(ticks);
        EXPECT_EQ(sph >> kSecondsFieldShift, 0u);
        EXPECT_LT(sph, kFirstTickAboveTheSphField);

        // Round trip through the shared decoder: the encoded value stands for
        // exactly the input's position within its second, negatives included.
        const int64_t expected =
            ((ticks % kTicksPerSecond) + kTicksPerSecond) % kTicksPerSecond;
        EXPECT_EQ(ASFW::Timing::encodedTstampToOffsets(sph), expected);
    }
}

// A host anchor carries whole seconds; the wire value must not. Without this,
// the seed's second leaks into the SPH and the device sees a field the original
// driver never sets.
TEST(MotuV3SphWireParityTests, HostSecondsDoNotReachTheWire) {
    constexpr int64_t kWithinSecond = 7999 * MotuV3Wire::kTicksPerCycle + 2800;
    for (int64_t second = 0; second < 130; ++second) {
        SCOPED_TRACE(second);
        EXPECT_EQ(MotuV3Wire::EncodeSph(second * kTicksPerSecond + kWithinSecond),
                  MotuV3Wire::EncodeSph(kWithinSecond));
    }
}

// The measurement side of the same domain. A device SPH rolling 7999 -> 0 is a
// step of one packet, not a jump backwards by nearly a second.
TEST(MotuV3SphWireParityTests, ShortestTickDifferenceCrossesTheSecondBoundary) {
    const int64_t before = kTicksPerSecond - 1024;
    const int64_t after = 3072;  // one cycle past the rollover
    EXPECT_EQ(ASFW::Audio::Runtime::MotuShortestTickDifference(after, before),
              4096);
    EXPECT_EQ(ASFW::Audio::Runtime::MotuShortestTickDifference(before, after),
              -4096);
}

// The P4 error signal is the difference of two quantities that are each ALREADY
// folded into +/-kTicksPerSecond/2: the TX presentation residual and the RX
// absolute phase. Subtracting them arithmetically therefore ranges over a full
// domain, and the first hardware run of `[RxPhaseRel]` recorded -6041600 that
// way while both inputs stayed in their normal few-thousand-tick band.
// The signal is modular; only a modular difference is bounded.
TEST(MotuV3SphWireParityTests, RelativePhaseStaysBoundedWhenTermsSitOnOppositeEdges) {
    constexpr int64_t kHalfDomain = kTicksPerSecond / 2;
    // Both are legal outputs of MotuShortestTickDifference, near opposite edges.
    const int64_t residualTicks = kHalfDomain - 1000;
    const int64_t rxAbsTicks = -(kHalfDomain - 1000);

    // What the defect did: a value larger than the domain itself.
    EXPECT_GT(residualTicks - rxAbsTicks, kTicksPerSecond - 2001);

    const int64_t rel = ASFW::Audio::Runtime::MotuShortestTickDifference(
        residualTicks, rxAbsTicks);
    EXPECT_GE(rel, -kHalfDomain);
    EXPECT_LE(rel, kHalfDomain);
    // The two edges are 2000 ticks apart the short way round, not a domain apart.
    EXPECT_EQ(rel, -2000);
}

// The healthy hardware case must survive the fold untouched: hardware measured
// `rel` standing at -662 with both terms a few thousand ticks, and a modular
// difference has to leave that arithmetic alone.
TEST(MotuV3SphWireParityTests, RelativePhaseIsPlainSubtractionWellInsideTheDomain) {
    EXPECT_EQ(ASFW::Audio::Runtime::MotuShortestTickDifference(3833, 4496), -663);
    EXPECT_EQ(ASFW::Audio::Runtime::MotuShortestTickDifference(3825, 4488), -663);
}

} // namespace
