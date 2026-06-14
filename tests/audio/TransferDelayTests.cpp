// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// TransferDelayTests.cpp
//
// Validates the IEC 61883-6 TRANSFER_DELAY math used to build/strip AMDTP SYT
// timestamps, and pins a known defect in the IAudioDeviceProfile default
// implementation (a flat constant that ignores the sample rate).
//
// Reference facts (IEC 61883-6 §7.3, IEEE 1394-2008 §8.3.2.3.1):
//   * The bus cycle-offset timer runs at a free-running 24.576 MHz clock, so
//     1 tick = 1/24.576 µs and 1 cycle = 3072 ticks = 125 µs.
//   * DEFAULT_TRANSFER_DELAY = (354.17 + 125) µs = 479.17 µs = 0x2E00 = 11776
//     ticks. This is exactly what Linux amdtp-stream.c uses (TRANSFER_DELAY_TICKS
//     = 0x2e00) and what ASFW stores in kTransferDelayTicks.
//   * For BLOCKING transmission the delay additionally absorbs the time the
//     transmitter spends accumulating one SYT_INTERVAL of data blocks, which
//     scales with the sample rate: TICKS_PER_SECOND * SYT_INTERVAL / rate.
//     amdtp-stream.c keeps the -1 cycle split because compute_syt's cycle
//     arithmetic re-supplies that cycle. ASFW mirrors this in
//     TxTimingModel::XmitTransferDelayTicksForRate().

#include "Audio/DriverKit/Config/IAudioDeviceProfile.hpp"
#include "Audio/Wire/AMDTP/RxSequenceReplay.hpp"
#include "Audio/Wire/AMDTP/TxTimingModel.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using ASFW::Driver::TxTimingModel;
using ASFW::Isoch::Audio::IAudioDeviceProfile;
namespace Replay = ASFW::Audio::Runtime;

// Spec-exact tick value of DEFAULT_TRANSFER_DELAY (479.17 µs @ 24.576 MHz).
constexpr uint32_t kIecBaselineTicks = 0x2E00;       // 11776
constexpr int64_t kTicksPerCycle = TxTimingModel::kTicksPerCycle; // 3072

// A minimal concrete profile that does NOT override the transfer-delay hooks,
// so we exercise the IAudioDeviceProfile base-class defaults under test.
class StubProfile final : public IAudioDeviceProfile {
public:
    const char* Name() const noexcept override { return "Stub"; }
    ASFW::Encoding::AudioWireFormat TxWireFormat() const noexcept override {
        return ASFW::Encoding::AudioWireFormat::kRawPcm24In32;
    }
    ASFW::Encoding::AudioWireFormat RxWireFormat() const noexcept override {
        return ASFW::Encoding::AudioWireFormat::kRawPcm24In32;
    }
    uint32_t TxChannelCount() const noexcept override { return 2; }
    uint32_t RxChannelCount() const noexcept override { return 2; }
    uint32_t TxMidiSlots() const noexcept override { return 0; }
    uint32_t RxMidiSlots() const noexcept override { return 0; }
    uint32_t TxDbs() const noexcept override { return 2; }
    uint32_t RxDbs() const noexcept override { return 2; }
    uint32_t TxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t RxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t TxReportedLatencyFrames(double) const noexcept override { return 0; }
    uint32_t RxReportedLatencyFrames(double) const noexcept override { return 0; }
};

// Ticks of the SYT presentation point within its 16-cycle window. The 4-bit
// cycle field plus the 12-bit offset field form a 16*3072 = 49152-tick domain.
[[nodiscard]] uint32_t DecodeSytWindowTicks(uint16_t syt) noexcept {
    const uint32_t cycleLow = (static_cast<uint32_t>(syt) >> 12) & 0x0Fu;
    const uint32_t offset = static_cast<uint32_t>(syt) & 0x0FFFu;
    return cycleLow * static_cast<uint32_t>(kTicksPerCycle) + offset;
}

// -----------------------------------------------------------------------------
// Group A — the IEC 61883-6 DEFAULT_TRANSFER_DELAY baseline.
// -----------------------------------------------------------------------------

TEST(TransferDelayBaseline, MatchesLinux0x2E00) {
    // Linux firewire amdtp-stream.c: #define TRANSFER_DELAY_TICKS 0x2e00
    EXPECT_EQ(ASFW::Timing::kTransferDelayTicks, kIecBaselineTicks);
    EXPECT_EQ(ASFW::Timing::kTransferDelayTicks, 11776u);
}

TEST(TransferDelayBaseline, BaselineIs479Microseconds) {
    // 11776 ticks / 24.576 ticks-per-µs = 479.166... µs.
    // kTransferDelayNanos = 11776 * 125000 / 3072 (integer) = 479166 ns.
    EXPECT_EQ(ASFW::Timing::kTransferDelayNanos, 479166u);
}

TEST(TransferDelayBaseline, TwelveEightHundredIsNotTheBaseline) {
    // 12800 ticks = 520.833 µs — a padded value, not the 479.17 µs baseline.
    // This documents that a literal 12800 is NOT the spec default; it is only
    // legitimate as the 48 kHz *blocking* total (see Group B).
    constexpr uint32_t kPadded = 12800;
    EXPECT_NE(kPadded, kIecBaselineTicks);
    const uint64_t paddedNanos = (uint64_t(kPadded) * 125000ull) / 3072ull;
    EXPECT_EQ(paddedNanos, 520833u);
}

// -----------------------------------------------------------------------------
// Group B — blocking-mode transfer delay scales with the sample rate.
// -----------------------------------------------------------------------------

TEST(BlockingTransferDelay, DecomposesIntoBaselineLessCyclePlusAccumulation) {
    // 48 kHz, SYT_INTERVAL = 8:
    //   baseline-less-cycle = 11776 - 3072 = 8704
    //   accumulation        = 24576000 * 8 / 48000 = 4096
    //   total               = 12800
    const int64_t baselineLessCycle = kIecBaselineTicks - kTicksPerCycle;
    EXPECT_EQ(baselineLessCycle, 8704);
    const int64_t accumulation =
        (TxTimingModel::kTicksPerSecond * 8) / 48000;
    EXPECT_EQ(accumulation, 4096);
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(8, 48000),
              baselineLessCycle + accumulation);
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(8, 48000), 12800);
}

TEST(BlockingTransferDelay, ConstantAcrossPowerOfTwoRateFamily) {
    // SYT_INTERVAL doubles with the rate family, so the accumulation term stays
    // 4096 ticks → 12800 total at 48/96/192 kHz.
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(8, 48000), 12800);
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(16, 96000), 12800);
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(32, 192000), 12800);
}

TEST(BlockingTransferDelay, ScalesAtFractionalRates) {
    // 44.1 kHz family: accumulation = 24576000 * INTERVAL / rate ≈ 4458 ticks.
    //   8704 + 4458 = 13162.
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(8, 44100), 13162);
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(16, 88200), 13162);
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(32, 176400), 13162);
}

TEST(BlockingTransferDelay, ScalesAt32k) {
    // 32 kHz: accumulation = 24576000 * 8 / 32000 = 6144 → 8704 + 6144 = 14848.
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(8, 32000), 14848);
}

TEST(BlockingTransferDelay, ZeroRateReturnsBaselineLessCycle) {
    EXPECT_EQ(TxTimingModel::XmitTransferDelayTicksForRate(8, 0), 8704);
}

TEST(BlockingTransferDelay, ConfigDefaultAgreesWith48kLadder) {
    EXPECT_EQ(TxTimingModel::Config{}.xmitTransferDelayTicks,
              TxTimingModel::XmitTransferDelayTicksForRate(8, 48000));
}

// -----------------------------------------------------------------------------
// Group C — the defect: IAudioDeviceProfile defaults ignore the sample rate.
//
// IAudioDeviceProfile::{Rx,Tx}TransferDelayTicks(double) currently do
// `(void)sampleRate; return 12800;`. That happens to equal the standard
// blocking delay ONLY for the 48/96/192 kHz family. At 44.1/32/88.2/176.4 kHz
// the flat constant diverges from both the IEC formula and the rate-scaled
// value that the lifecycle feeds into TxTimingModel — so the two TX SYT
// authorities disagree at those rates.
// -----------------------------------------------------------------------------

TEST(ProfileDefaultTransferDelay, CorrectAtPowerOfTwoRateFamily) {
    StubProfile profile;
    // At the bring-up rate family the flat default is coincidentally correct.
    EXPECT_EQ(profile.TxTransferDelayTicks(48000.0),
              static_cast<uint32_t>(
                  TxTimingModel::XmitTransferDelayTicksForRate(8, 48000)));
    EXPECT_EQ(profile.RxTransferDelayTicks(48000.0), 12800u);
    EXPECT_EQ(profile.TxTransferDelayTicks(96000.0), 12800u);
    EXPECT_EQ(profile.TxTransferDelayTicks(192000.0), 12800u);
}

TEST(ProfileDefaultTransferDelay, DivergesFromStandardAtFractionalRates_KnownBug) {
    StubProfile profile;

    // The rate-correct blocking delay the TxTimingModel path uses at 44.1 kHz.
    const uint32_t standard44k = static_cast<uint32_t>(
        TxTimingModel::XmitTransferDelayTicksForRate(8, 44100));
    ASSERT_EQ(standard44k, 13162u);

    // BUG: the default profile returns the flat 48 kHz value regardless of rate.
    // These EXPECT_NE / EXPECT_EQ assertions PIN the current (incorrect)
    // behavior. When the default is fixed to delegate to the rate-scaled
    // formula, flip them to EXPECT_EQ(..., standard44k) — they will start
    // failing here as the prompt to do so.
    EXPECT_EQ(profile.TxTransferDelayTicks(44100.0), 12800u);
    EXPECT_NE(profile.TxTransferDelayTicks(44100.0), standard44k);
    EXPECT_EQ(profile.RxTransferDelayTicks(44100.0), 12800u);
    EXPECT_NE(profile.RxTransferDelayTicks(44100.0), standard44k);

    // The miss is 362 ticks (~14.7 µs) of presentation error at 44.1 kHz.
    EXPECT_EQ(standard44k - profile.TxTransferDelayTicks(44100.0), 362u);

    // 32 kHz is worse: 14848 vs 12800 = 2048 ticks (~83.3 µs).
    const uint32_t standard32k = static_cast<uint32_t>(
        TxTimingModel::XmitTransferDelayTicksForRate(8, 32000));
    EXPECT_EQ(standard32k - profile.TxTransferDelayTicks(32000.0), 2048u);
}

TEST(ProfileDefaultTransferDelay, ContradictsTxTimingModelWiringAt44k) {
    // Reproduces the two code paths ASFWAudioDriverLifecycle wires at start:
    //   * TxTimingModel.xmitTransferDelayTicks  <- XmitTransferDelayTicksForRate
    //   * control->txTransferDelayTicks          <- profile->TxTransferDelayTicks
    // They MUST agree (the same wire SYT presentation delay) but do not at 44.1k.
    StubProfile profile;
    const double rate = 44100.0;

    const int64_t timingModelDelay =
        TxTimingModel::XmitTransferDelayTicksForRate(8,
                                                     static_cast<uint32_t>(rate));
    const uint32_t controlBlockDelay = profile.TxTransferDelayTicks(rate);

    EXPECT_NE(static_cast<uint32_t>(timingModelDelay), controlBlockDelay)
        << "Known bug: the two TX SYT authorities disagree at 44.1 kHz";

    // At 48 kHz they DO agree — which is why the bug is invisible on the
    // 48 kHz bring-up bench.
    EXPECT_EQ(static_cast<uint32_t>(
                  TxTimingModel::XmitTransferDelayTicksForRate(8, 48000)),
              profile.TxTransferDelayTicks(48000.0));
}

// -----------------------------------------------------------------------------
// Group D — replay strip/re-add invariants (why a *consistent* delay matters).
// -----------------------------------------------------------------------------

TEST(ReplayTransferDelay, StripThenReAddWithEqualDelayRecoversSyt) {
    // ComputeReplaySytOffset strips the delay; ComputeReplaySyt re-adds it.
    // With the same delay and matching source/output cycle, the value the delay
    // contributes cancels exactly — for any delay value.
    const uint32_t sourceTimer = ASFW::Timing::encodeCycleTimer(0, 1, 0);
    for (uint32_t delay : {kIecBaselineTicks, 12800u, 13162u, 14848u}) {
        for (uint16_t syt : {uint16_t(0x3ABC), uint16_t(0x1000),
                             uint16_t(0xF0FF), uint16_t(0x07FF)}) {
            const uint32_t offset =
                Replay::ComputeReplaySytOffset(syt, sourceTimer, delay);
            const uint16_t recovered =
                Replay::ComputeReplaySyt(offset, sourceTimer, delay);
            EXPECT_EQ(recovered, syt)
                << "delay=" << delay << " syt=" << syt;
        }
    }
}

TEST(ReplayTransferDelay, MismatchedDelayShiftsPresentationByExactlyTheDelta) {
    // If the strip delay (rx) and the re-add delay (tx) differ — exactly the
    // hazard the Group C bug creates between code paths — the recovered SYT
    // presentation shifts by (txDelay - rxDelay) ticks.
    const uint32_t sourceTimer = ASFW::Timing::encodeCycleTimer(0, 1, 0);
    const uint16_t syt = 0x3ABC;
    const uint32_t rxDelay = 12800;   // flat profile default
    const uint32_t txDelay = 13162;   // rate-correct 44.1 kHz value
    const int64_t expectedShift = int64_t(txDelay) - int64_t(rxDelay); // 362

    const uint32_t offset =
        Replay::ComputeReplaySytOffset(syt, sourceTimer, rxDelay);
    const uint16_t matched =
        Replay::ComputeReplaySyt(offset, sourceTimer, rxDelay);
    const uint16_t mismatched =
        Replay::ComputeReplaySyt(offset, sourceTimer, txDelay);

    EXPECT_NE(matched, mismatched);
    const int64_t observedShift =
        int64_t(DecodeSytWindowTicks(mismatched)) -
        int64_t(DecodeSytWindowTicks(matched));
    EXPECT_EQ(observedShift, expectedShift);
}

} // namespace
