#pragma once

#include "AudioHalBufferProfiles.hpp"

#include <cstdint>

namespace ASFW::IsochTransport {

struct AudioTimingGeometry final {
    static constexpr uint32_t kSampleRateHz = 48000;

    // Blocking AMDTP cadence at 48k: D,D,D,N over 4 packets, 8 frames per
    // data packet => 24 frames per cadence block (6 frames/packet average).
    static constexpr uint32_t kFramesPerDataPacket = 8;
    static constexpr uint32_t kCadenceBlockPackets = 4;
    static constexpr uint32_t kCadenceBlockFrames = 24;

    // DMA completion cadence is deliberately independent from the HAL ZTS
    // grid. Six FireWire cycles give 0.75 ms refill latency. Depending on the
    // D/D/D/N starting phase, one interrupt carries 32 or 40 decoded frames.
    static constexpr uint32_t kRxPacketsPerGroup = 6;
    static constexpr uint32_t kTxPacketsPerGroup = 6;
    static constexpr uint32_t kTimingGroupPackets = kRxPacketsPerGroup;

    static constexpr uint32_t kMinimumNominalFramesPerInterrupt = 32;
    static constexpr uint32_t kMaximumNominalFramesPerInterrupt = 40;
    static constexpr uint32_t kNominalFramesPerTimingGroup =
        36;

    // HAL-facing geometry is selected as one compile-time profile because the
    // frame ring sizes cross-process shared memory.
    static constexpr uint32_t kFrameRingFrames =
        kActiveAudioHalBufferProfile.frameRingFrames;
    static constexpr uint32_t kHalZeroTimestampPeriodFrames =
        kActiveAudioHalBufferProfile.zeroTimestampPeriodFrames;

    // The graph applies the complete profile/output/client-IO formula. This is
    // only the interrupt-batch component of that calculation.
    static constexpr uint32_t kInputSafetyFloorFrames =
        kMaximumNominalFramesPerInterrupt + 64;

    // Client IO sizing/safety budget. ADK may issue a different operation
    // span; the callback validates that span against stream-ring capacity.
    static constexpr uint32_t kHalIoPeriodFrames =
        kActiveAudioHalBufferProfile.clientIoBudgetFrames;

    static constexpr uint32_t kFrameAlignment = 32;

    static constexpr uint32_t kRxDescriptorPackets = 504;

    // Packet-domain TX ownership: 48 descriptors on hardware, plus a committed
    // preparation lead beyond the hardware ring.
    //
    // COVERAGE INVARIANT (hardware-confirmed). The IT refill ISR checks slots
    //   [completion + hardwareRing, completion + hardwareRing + deltaConsumed)
    // and FATALs if any is not yet committed. The producer stays `slack` packets
    // ahead of the hardware ring (lead = hardwareRing + slack), so a single
    // refill tolerates a coalesced completion of at most `slack` packets:
    //   lead - hardwareRing == slack  >=  max(deltaConsumed)
    // This bound covers both a coalesced refill and packets consumed while the
    // cross-process producer action is delayed. Field runs exhausted 36 packets
    // of slack after 40-42 packets (5.0-5.25 ms) elapsed without a producer
    // wake, even with a dedicated DriverKit queue. Keep two hardware-ring
    // depths of slack (96 packets / 12 ms), giving the active lead three
    // hardware-ring depths while leaving one full hardware-ring depth before
    // shared-slot reuse. See
    // documentation/ZTS_AND_SYT.md §13 and tests/audio/TxRefillCoverageTests.cpp.
    static constexpr uint32_t kTxSharedSlotPackets = 192;
    static constexpr uint32_t kTxHardwareRingPackets = 48;
    static constexpr uint32_t kTxPreparationSlackPackets =
        2 * kTxHardwareRingPackets;
    static constexpr uint32_t kTxPreparationLeadPackets =
        kTxHardwareRingPackets + kTxPreparationSlackPackets;
    // Largest single coalesced deltaConsumed a refill can absorb without holing.
    static constexpr uint32_t kTxMaxCoveredDeltaConsumedPackets =
        kTxPreparationSlackPackets;
};

static_assert(AudioTimingGeometry::kRxDescriptorPackets %
                  AudioTimingGeometry::kTimingGroupPackets ==
              0);
static_assert(AudioTimingGeometry::kRxDescriptorPackets %
                  AudioTimingGeometry::kCadenceBlockPackets ==
              0);
static_assert(AudioTimingGeometry::kRxDescriptorPackets % 12 == 0);
static_assert(AudioTimingGeometry::kFrameRingFrames %
                  AudioTimingGeometry::kHalIoPeriodFrames ==
              0,
              "Frame ring must be an integer number of max HAL IO periods");
static_assert(AudioTimingGeometry::kFrameRingFrames %
                  AudioTimingGeometry::kHalZeroTimestampPeriodFrames ==
              0,
              "Frame ring must be an integer number of ZTS periods");
static_assert(AudioTimingGeometry::kFrameRingFrames %
                  AudioTimingGeometry::kFrameAlignment ==
              0,
              "Frame ring must satisfy the 32-frame alignment contract");
static_assert(AudioTimingGeometry::kFrameRingFrames >=
                  AudioTimingGeometry::kHalIoPeriodFrames,
              "Frame ring must hold one maximum HAL IO transfer");
static_assert(AudioTimingGeometry::kTimingGroupPackets != 0,
              "Timing group packet count must be non-zero");
static_assert(AudioTimingGeometry::kRxPacketsPerGroup ==
                  AudioTimingGeometry::kTxPacketsPerGroup,
              "RX and TX interrupt groups must match");
static_assert(AudioTimingGeometry::kTxSharedSlotPackets >=
              AudioTimingGeometry::kTxPreparationLeadPackets,
              "TX shared slot ring must hold the full preparation lead");
// COVERAGE: tolerate 16 six-packet groups without a producer wake. This covers
// the observed 40-42 packet DriverKit dispatch stalls with more than 2x margin.
static_assert(AudioTimingGeometry::kTxMaxCoveredDeltaConsumedPackets >=
                  16 * AudioTimingGeometry::kTxPacketsPerGroup,
              "TX preparation slack must cover at least 12 ms of producer "
              "dispatch latency at the current six-packet cadence");
static_assert(AudioTimingGeometry::kTxPreparationLeadPackets <=
                  AudioTimingGeometry::kTxSharedSlotPackets -
                      AudioTimingGeometry::kTxHardwareRingPackets,
              "TX preparation must leave one hardware-ring depth before "
              "shared-slot reuse");
static_assert(AudioTimingGeometry::kTxSharedSlotPackets %
                  AudioTimingGeometry::kTxPacketsPerGroup ==
              0,
              "TX shared slot ring must be an integer number of groups");
static_assert(AudioTimingGeometry::kTxHardwareRingPackets %
                  AudioTimingGeometry::kTxPacketsPerGroup ==
              0,
              "TX hardware ring must be an integer number of groups");
static_assert(AudioTimingGeometry::kTxHardwareRingPackets %
                  AudioTimingGeometry::kCadenceBlockPackets ==
              0,
              "TX ring wrap must preserve blocking-cadence phase");

} // namespace ASFW::IsochTransport
