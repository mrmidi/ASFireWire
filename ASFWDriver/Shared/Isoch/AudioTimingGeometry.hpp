#pragma once

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

    // HAL-facing PCM frame ring (one per direction, directly mapped).
    // AudioDriverKit's sample allocates one complete ring per declared zero
    // timestamp period. Keep those values identical so the host and driver
    // agree on the stream-buffer wrap point.
    static constexpr uint32_t kFrameRingFrames = 1536;

    // The declared HAL clock period is also the mapped stream-ring length.
    // RX still runs at the shorter DMA cadence and publishes only when its
    // decoded-frame cursor crosses this grid.
    static constexpr uint32_t kHalZeroTimestampPeriodFrames =
        kFrameRingFrames;

    // The graph applies the complete profile/output/client-IO formula. This is
    // only the interrupt-batch component of that calculation.
    static constexpr uint32_t kInputSafetyFloorFrames =
        kMaximumNominalFramesPerInterrupt + 64;

    // Upper bound on a single HAL IO transfer (client buffer size).
    static constexpr uint32_t kHalIoPeriodFrames = 512;

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
    // This is a COVERAGE bound, not a latency margin: producer wake latency was
    // measured at <100 us ([TxPrep]) yet slack == 2*group (=12) still holed on a
    // single deltaConsumed=13 coalesced IT completion (IT FATAL dump,
    // fatalAbs=1332, slotLastPacketAbs one lap behind, exposeCursor at target).
    // The IT refill ISR coalesces several completion groups under DriverKit
    // scheduling (RX is observed coalescing up to 6 groups), so size the slack
    // for the worst expected coalescing. 6*group == 36 covers a six-group
    // coalesced completion; raise further if the maxDeltaConsumed high-water
    // counter (counters_.maxDeltaConsumed) reports more. See
    // documentation/ZTS_AND_SYT.md §13 and tests/audio/TxRefillCoverageTests.cpp.
    static constexpr uint32_t kTxSharedSlotPackets = 192;
    static constexpr uint32_t kTxHardwareRingPackets = 48;
    static constexpr uint32_t kTxPreparationSlackPackets =
        6 * kTxPacketsPerGroup;
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
static_assert(AudioTimingGeometry::kFrameRingFrames ==
                  AudioTimingGeometry::kHalZeroTimestampPeriodFrames,
              "AudioDriverKit stream ring must equal the declared ZTS period");
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
// COVERAGE: a single refill absorbs a coalesced completion of at most `slack`
// packets. Hardware showed deltaConsumed=13 holing slack==12, so require slack
// to cover several groups of coalescing.
static_assert(AudioTimingGeometry::kTxMaxCoveredDeltaConsumedPackets >=
                  6 * AudioTimingGeometry::kTxPacketsPerGroup,
              "TX preparation slack must cover the worst expected coalesced "
              "IT completion (>= 6 groups); raise if maxDeltaConsumed exceeds it");
static_assert(AudioTimingGeometry::kTxPreparationLeadPackets <=
                  AudioTimingGeometry::kTxSharedSlotPackets -
                      AudioTimingGeometry::kTxPacketsPerGroup,
              "TX preparation must not overwrite the next uncompleted group");
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
