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

    // Hardware interrupt cadence: one IR/IT interrupt every 32 packets
    // (4 ms @48k). 32 packets = 8 whole cadence blocks, so the nominal frame
    // advance per interrupt is constant (192) and the ZTS grid is aligned
    // 1:1 with the DMA interrupt program — one anchor per interrupt at the
    // group boundary, no synthesized mid-group anchors.
    static constexpr uint32_t kRxPacketsPerGroup = 32;
    static constexpr uint32_t kTxPacketsPerGroup = 32;
    static constexpr uint32_t kTimingGroupPackets = kRxPacketsPerGroup;

    // Nominal frame advance per interrupt group when the device tracks the
    // bus rate exactly. Real device crystals drift (ppm), so an individual
    // group occasionally advances one data packet more or less; the RX drain
    // therefore publishes anchors when the cursor crosses the declared grid
    // (which in the nominal cadence is exactly the interrupt boundary).
    static constexpr uint32_t kNominalFramesPerTimingGroup =
        (kTimingGroupPackets / kCadenceBlockPackets) * kCadenceBlockFrames;

    // Zero-timestamp grid the HAL predicts against (ADK contract: next
    // anchor sample time = previous + period). Equal to the per-interrupt
    // frame advance: the interrupt IS the ZTS callback (doc §9.3 case 3).
    static constexpr uint32_t kHalZeroTimestampPeriodFrames =
        kNominalFramesPerTimingGroup;

    // Captured input reaches the HAL-facing ring only at the IR drain, i.e.
    // in one-group batches. The declared input safety offset must therefore
    // cover one full group plus scheduling jitter, whatever the device
    // profile says (Saffire's decompiled 128 assumed its 1.5 ms groups).
    static constexpr uint32_t kInputSafetyFloorFrames =
        kNominalFramesPerTimingGroup + 64;

    // Upper bound on a single HAL IO transfer (client buffer size).
    static constexpr uint32_t kHalIoPeriodFrames = 512;

    // HAL-facing PCM frame ring (one per direction, directly mapped).
    // Must be an integer multiple of the ZTS period, the max IO period, and
    // the nominal group advance, and must exceed max IO + output safety so a
    // full-size client write can never lap the hardware cursor (the old
    // ring == max IO == 512 geometry had zero output headroom). Ring size
    // does not add latency — the safety offsets govern that.
    static constexpr uint32_t kFrameRingFrames = 1536;

    static constexpr uint32_t kFrameAlignment = 32;

    // TX shared packet rings (payload slab + metadata), packet domain.
    // Independent of the frame ring: sized to hold the full preparation lead
    // with headroom. 512 packets = 64 ms @ 8000 pkt/s.
    static constexpr uint32_t kTxSharedSlotPackets = 512;

    static constexpr uint32_t kTxHardwareRingPackets = 192;
    // Keep two completion groups prepared beyond the hardware-owned ring.
    // One group is consumed by the normal refill itself; the second gives
    // the cross-driver preparation action one full additional period to run.
    // At 48 kHz this is 64 packets / 384 frames / 8 ms, matching the output
    // preparation deadline declared by TimingCursorPolicy.
    static constexpr uint32_t kTxPreparationSlackPackets =
        2 * kTxPacketsPerGroup;
    static constexpr uint32_t kTxPreparationLeadPackets =
        kTxHardwareRingPackets + kTxPreparationSlackPackets;
};

static_assert(AudioTimingGeometry::kTimingGroupPackets %
                  AudioTimingGeometry::kCadenceBlockPackets ==
              0,
              "Interrupt group must not cut through the D/D/D/N cadence block");
static_assert(AudioTimingGeometry::kHalZeroTimestampPeriodFrames %
                  AudioTimingGeometry::kNominalFramesPerTimingGroup ==
              0,
              "ZTS grid crossings must land on interrupt-group boundaries in "
              "the nominal cadence (no mid-group anchor synthesis)");
static_assert(AudioTimingGeometry::kFrameRingFrames %
                  AudioTimingGeometry::kHalIoPeriodFrames ==
              0,
              "Frame ring must be an integer number of max HAL IO periods");
static_assert(AudioTimingGeometry::kFrameRingFrames %
                  AudioTimingGeometry::kHalZeroTimestampPeriodFrames ==
              0,
              "Frame ring must be an integer number of ZTS periods");
static_assert(AudioTimingGeometry::kFrameRingFrames %
                  AudioTimingGeometry::kNominalFramesPerTimingGroup ==
              0,
              "Frame ring must be an integer number of interrupt groups");
static_assert(AudioTimingGeometry::kFrameRingFrames %
                  AudioTimingGeometry::kFrameAlignment ==
              0,
              "Frame ring must satisfy the 32-frame alignment contract");
static_assert(AudioTimingGeometry::kFrameRingFrames >=
              AudioTimingGeometry::kHalIoPeriodFrames +
                  AudioTimingGeometry::kHalZeroTimestampPeriodFrames,
              "Frame ring needs headroom beyond one max IO transfer");
static_assert(AudioTimingGeometry::kTimingGroupPackets != 0,
              "Timing group packet count must be non-zero");
static_assert(AudioTimingGeometry::kRxPacketsPerGroup ==
                  AudioTimingGeometry::kTxPacketsPerGroup,
              "RX and TX interrupt groups must match");
static_assert(AudioTimingGeometry::kTxSharedSlotPackets >=
              AudioTimingGeometry::kTxPreparationLeadPackets,
              "TX shared slot ring must hold the full preparation lead");
static_assert(AudioTimingGeometry::kTxPreparationSlackPackets >=
                  2 * AudioTimingGeometry::kTxPacketsPerGroup,
              "TX preparation must tolerate one delayed completion wake");
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

} // namespace ASFW::IsochTransport
