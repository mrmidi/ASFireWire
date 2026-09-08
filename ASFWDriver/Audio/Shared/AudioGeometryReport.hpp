// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioGeometryReport.hpp
//
// Everything the Audio Geometry panel DISPLAYS but cannot change: the
// compile-time structure from AudioTimingGeometry.hpp plus the values that are
// functions of the running sample rate from AudioGeometryPolicy.hpp, gathered
// into one struct.
//
// Why this exists as its own header rather than being assembled inline where
// the wire snapshot is filled: that site is a DriverKit translation unit the
// host tests cannot compile, so every derived number the operator reads would
// be untested arithmetic. Deriving it here means the interrupt cadence, the
// per-rate frame counts and the tuning bounds are all covered by
// tests/audio/AudioGeometryReportTests.cpp, and the driver-side fill is a
// mechanical copy.
//
// It also exists so the *app* never re-derives any of this. A panel that
// hardcodes "72 packets" or "6 frames per packet" is a second source of truth
// that silently disagrees with the driver the moment a constant moves; every
// number below is published so the app can render it rather than compute it.
// The one exception is a rate that is not an integer -- interrupts per second
// is 8000/6, so this publishes the exact interval in microseconds and lets the
// presentation layer take the reciprocal.

#pragma once

#include "AudioGeometryPolicy.hpp"
#include "AudioTimingGeometry.hpp"

#include <algorithm>
#include <cstdint>

namespace ASFW::Audio::Shared {

// DATA packets inside one completion group, worst and best phase of the
// blocking cadence. Computed by scanning every starting phase rather than
// assuming the 48 kHz answer, so the numbers stay right if the cadence block
// or the group size moves. At the shipping 4-packet D,D,D,N block a six-packet
// group straddles either one or two NO-DATA slots, hence 4 or 5 DATA packets.
struct CadenceWindowDataPackets final {
    uint32_t minimum{0};
    uint32_t maximum{0};
};

[[nodiscard]] constexpr CadenceWindowDataPackets DataPacketsInWindow(
    uint32_t windowPackets, uint32_t cadenceBlockPackets,
    uint32_t dataPacketsPerBlock) noexcept {
    if (windowPackets == 0 || cadenceBlockPackets == 0 ||
        dataPacketsPerBlock > cadenceBlockPackets) {
        return {};
    }
    CadenceWindowDataPackets out{windowPackets, 0};
    for (uint32_t phase = 0; phase < cadenceBlockPackets; ++phase) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < windowPackets; ++i) {
            // Slots [0, dataPacketsPerBlock) of each block carry DATA; the
            // remainder are NO-DATA. Which slot is idle is a phase choice, not
            // a policy -- only how many are idle matters here.
            if ((phase + i) % cadenceBlockPackets < dataPacketsPerBlock) {
                ++count;
            }
        }
        out.minimum = std::min(out.minimum, count);
        out.maximum = std::max(out.maximum, count);
    }
    return out;
}

struct AudioGeometryReport final {
    // --- the isochronous cycle grid ------------------------------------------
    uint32_t isochCyclesPerSecond{0};
    uint32_t microsecondsPerIsochCycle{0};

    // --- blocking AMDTP cadence (rate dependent) -----------------------------
    uint32_t framesPerDataPacket{0};
    uint32_t cadenceBlockPackets{0};
    uint32_t cadenceBlockFrames{0};

    // --- completion / interrupt cadence --------------------------------------
    // The interval is exact; the rate is not (8000/6 per second), so the
    // reciprocal is left to the presentation layer.
    uint32_t txPacketsPerGroup{0};
    uint32_t rxPacketsPerGroup{0};
    uint32_t txInterruptIntervalMicroseconds{0};
    uint32_t rxInterruptIntervalMicroseconds{0};
    uint32_t framesPerCompletionGroupTx{0};
    uint32_t framesPerCompletionGroupRx{0};
    // What one RX completion actually hands the decoder, which swings with the
    // cadence phase. The nominal above is the average, never an observation.
    uint32_t minFramesPerRxInterrupt{0};
    uint32_t maxFramesPerRxInterrupt{0};

    // --- isochronous rings ---------------------------------------------------
    uint32_t txHardwareRingPackets{0};
    uint32_t txSharedSlotPackets{0};
    uint32_t rxHardwareRingPackets{0};
    uint32_t txContentFreezePackets{0};
    uint32_t txRepointGuardPackets{0};
    // Audio frames in one traversal of the TX hardware ring. This is the
    // quantum of the observed round-trip lattice: measured RTL lands on
    // base + k * this, and the committed-margin minimum moves in the same
    // step, because a lap is the period over which the producer's position
    // relative to the OHCI CommandPtr repeats. Naming it here is what makes a
    // 288-frame jump in a measurement recognisable instead of mysterious.
    uint32_t txRingLapFrames{0};

    // --- host-side buffer structure ------------------------------------------
    uint32_t schedulingJitterFrames{0};
    uint32_t frameAlignmentFrames{0};
    uint32_t pcmPublicationCacheFrames{0};
    uint32_t maxBlockingFramesPerDataPacket{0};

    // --- bounds on what the panel may request --------------------------------
    uint32_t txDispatchSlackFloorPackets{0};
    uint32_t txDispatchSlackDefaultPackets{0};

    // --- per-rate policy values ----------------------------------------------
    // What AudioGeometryPolicy would derive at this rate. A device profile may
    // declare more (its own floor wins); it is shown so a declaration that sits
    // below the geometry can be recognised as such.
    uint32_t txSafetyOffsetPolicyFrames{0};
    uint32_t rxSafetyOffsetPolicyFrames{0};
    uint32_t reportedLatencyPolicyFrames{0};
    uint32_t completionBatchFrames{0};
};

// `sampleRateHz` of zero, or any rate outside the V3 family, leaves every
// rate-dependent field zero rather than quietly reporting the 48 kHz answer:
// the panel must be able to tell "not streaming yet" from "6 frames a packet".
[[nodiscard]] constexpr AudioGeometryReport DeriveGeometryReport(
    uint32_t sampleRateHz) noexcept {
    using G = AudioTimingGeometry;
    using P = AudioGeometryPolicy;

    AudioGeometryReport out{};
    out.isochCyclesPerSecond = G::kIsochCyclesPerSecond;
    out.microsecondsPerIsochCycle = G::kMicrosecondsPerIsochCycle;

    out.txPacketsPerGroup = G::kTxPacketsPerGroup;
    out.rxPacketsPerGroup = G::kRxPacketsPerGroup;
    out.txInterruptIntervalMicroseconds =
        G::kTxPacketsPerGroup * G::kMicrosecondsPerIsochCycle;
    out.rxInterruptIntervalMicroseconds =
        G::kRxPacketsPerGroup * G::kMicrosecondsPerIsochCycle;

    out.txHardwareRingPackets = G::kTxHardwareRingPackets;
    out.txSharedSlotPackets = G::kTxSharedSlotPackets;
    out.rxHardwareRingPackets = G::kRxHardwareRingPackets;
    out.txContentFreezePackets = G::kTxContentFreezeCycleSlots;
    out.txRepointGuardPackets = G::kTxDescriptorRepointGuardCycleSlots;

    out.schedulingJitterFrames = G::kSchedulingJitterFrames;
    out.frameAlignmentFrames = G::kFrameAlignment;
    out.pcmPublicationCacheFrames = G::kPcmPublicationCacheFrames;
    out.maxBlockingFramesPerDataPacket = kMaxBlockingFramesPerDataPacket;

    out.txDispatchSlackFloorPackets = G::kTxDispatchSlackFloorPackets;
    out.txDispatchSlackDefaultPackets = G::kTxDispatchSlackCycleSlots;

    out.cadenceBlockPackets = G::kCadenceBlockPackets;

    if (!G::IsV3SampleRate(sampleRateHz)) {
        return out;
    }

    const double rate = static_cast<double>(sampleRateHz);
    const uint32_t framesPerDataPacket = P::FramesPerPacket(rate);
    out.framesPerDataPacket = framesPerDataPacket;

    // Exact for the V3 family: every supported rate divides the cycle grid.
    out.framesPerCompletionGroupTx =
        G::kTxPacketsPerGroup * sampleRateHz / G::kIsochCyclesPerSecond;
    out.txRingLapFrames =
        G::kTxHardwareRingPackets * sampleRateHz / G::kIsochCyclesPerSecond;
    out.framesPerCompletionGroupRx =
        G::kRxPacketsPerGroup * sampleRateHz / G::kIsochCyclesPerSecond;
    out.cadenceBlockFrames =
        G::kCadenceBlockPackets * sampleRateHz / G::kIsochCyclesPerSecond;

    // DATA packets per cadence block follow from the frames the block carries,
    // so a cadence change moves this without a second constant to maintain.
    const uint32_t dataPacketsPerBlock =
        out.cadenceBlockFrames / framesPerDataPacket;
    const auto window = DataPacketsInWindow(
        G::kRxPacketsPerGroup, G::kCadenceBlockPackets, dataPacketsPerBlock);
    out.minFramesPerRxInterrupt = window.minimum * framesPerDataPacket;
    out.maxFramesPerRxInterrupt = window.maximum * framesPerDataPacket;

    out.txSafetyOffsetPolicyFrames = P::TxSafetyOffsetFrames(rate);
    out.rxSafetyOffsetPolicyFrames = P::RxSafetyOffsetFrames(rate);
    out.reportedLatencyPolicyFrames = P::ReportedLatencyFrames(rate);
    out.completionBatchFrames = P::CompletionBatchFrames(rate);
    return out;
}

// Pin the 48 kHz answers that the rest of the stack already asserts, so a
// refactor here cannot drift from AudioTimingGeometry's own constants.
static_assert(DeriveGeometryReport(48'000).framesPerCompletionGroupRx ==
                  AudioTimingGeometry::kNominalFramesPerTimingGroup,
              "nominal frames per group must match the timing geometry");
static_assert(DeriveGeometryReport(48'000).minFramesPerRxInterrupt ==
                  AudioTimingGeometry::kMinimumNominalFramesPerInterrupt,
              "48k RX interrupt floor must be 4 DATA packets");
static_assert(DeriveGeometryReport(48'000).maxFramesPerRxInterrupt ==
                  AudioTimingGeometry::kMaximumNominalFramesPerInterrupt,
              "48k RX interrupt ceiling must be 5 DATA packets");
static_assert(DeriveGeometryReport(48'000).cadenceBlockFrames ==
                  AudioTimingGeometry::kCadenceBlockFrames,
              "48k cadence block must be 24 frames");
static_assert(DeriveGeometryReport(48'000).framesPerDataPacket ==
                  AudioTimingGeometry::kFramesPerDataPacket,
              "48k blocking SYT interval must be 8 frames");
static_assert(DeriveGeometryReport(48'000).txInterruptIntervalMicroseconds ==
                  1'000,
              "eight-packet completion groups are 1.0 ms apart");
// 504 packets of hardware ring at six frames a packet. Historical note for
// anyone re-reading old RTL captures: this was 288 while the ring was 48, and
// the lattice quantum in those measurements is that number, not this one.
static_assert(DeriveGeometryReport(48'000).txRingLapFrames == 3'024,
              "48k TX ring lap must be 3024 frames");
static_assert(DeriveGeometryReport(96'000).txRingLapFrames == 6'048,
              "the lattice quantum scales with the rate, not the ring");
static_assert(DeriveGeometryReport(0).framesPerDataPacket == 0,
              "an unknown rate must not report the 48 kHz cadence");
static_assert(DeriveGeometryReport(0).txDispatchSlackFloorPackets ==
                  AudioTimingGeometry::kTxDispatchSlackFloorPackets,
              "rate-independent structure is reportable before streaming");

} // namespace ASFW::Audio::Shared
