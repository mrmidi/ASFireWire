#pragma once

#include "AudioHalBufferProfiles.hpp"

#include <cstdint>

namespace ASFW::IsochTransport {

// AUDIO-ONLY CONTRACT.  Do not include this header from Hardware/, Bus/,
// Async/, or Isoch/: those layers carry opaque isoch packets and must not
// inherit audio cadence, frame, buffer, or clock policy.  Generic OHCI
// descriptor geometry belongs in Isoch/Core/IsochDmaGeometry.hpp.

// -----------------------------------------------------------------------------
// UNIT DISCIPLINE: every constant carries its domain in the name --
//   ...Packets  FireWire isoch packets (one per 125 us cycle, 8000/s)
//   ...Frames   audio sample frames (48000/s at 1x)
//   ...Blocks   OHCI descriptor blocks (4 per TX packet, Z=4)
//   ...Ticks    24.576 MHz FireWire ticks (3072/cycle)
// Never compare a *Packets value to a *Frames value without an explicit
// conversion (the cadence is the only bridge: 6 frames/packet average at 48k,
// 5.5125 at 44.1k).
//
// TX OWNERSHIP: PCM reaches packets through the audio-side fill, from the
// HAL output ring, once (documentation/TX_OWNERSHIP.md). There is no
// frame-exposure frontier and no W/E cushion any more (T4, T8).
//
// Rate-DEPENDENT geometry (frames-per-packet, safety floor, declarations,
// transfer delay) is resolved per rate by Audio/Runtime/ResolvedTimingGeometry.hpp
// (documentation/TIMING_GEOMETRY_OWNERSHIP.md), not here.
// -----------------------------------------------------------------------------
struct AudioTimingGeometry final {
    static constexpr uint32_t kSampleRateHz = 48000;

    // Blocking AMDTP cadence at 48k: D,D,D,N over 4 packets, 8 frames per
    // data packet => 24 frames per cadence block (6 frames/packet average).
    static constexpr uint32_t kFramesPerDataPacket = 8;
    static constexpr uint32_t kCadenceBlockPackets = 4;
    static constexpr uint32_t kCadenceBlockFrames = 24;

    // DMA completion cadence: one interrupt per 8 FireWire cycles (1.0 ms),
    // the AppleFWAudio fNumPacketsPerBufferGroup value (see REFERENCE GEOMETRY
    // below). Eight packets are two whole blocking cadence blocks, so every
    // group carries exactly 6 DATA packets whatever its starting phase:
    // 48 frames at 1x, 96 at 2x, 192 at 4x. A 6-packet group straddled the
    // D/D/D/N block (32 or 40 frames) and did not tile the ZTS period
    // (12288 / 36 is not an integer), which is why the group moved to 8.
    static constexpr uint32_t kRxPacketsPerGroup = 8;
    static constexpr uint32_t kTxPacketsPerGroup = 8;
    static constexpr uint32_t kTimingGroupPackets = kRxPacketsPerGroup;

    // Fixed-phase frames per completion group at 48 kHz. Rate-general value:
    // CompletionBatchFrames() in Audio/Runtime/ResolvedTimingGeometry.hpp.
    static constexpr uint32_t kNominalFramesPerTimingGroup =
        (kTimingGroupPackets / kCadenceBlockPackets) * kCadenceBlockFrames; // 48

    // HAL frame ring and ZTS period are per rate (HalBufferProfileForRate,
    // AudioHalBufferProfiles.hpp); only the shared-memory allocation is fixed.
    static constexpr uint32_t kAllocatedFrameRingFrames =
        ::ASFW::IsochTransport::kAllocatedFrameRingFrames;

    // Nominal client IO budget (the profile's clientIoBudgetFrames). ADK may
    // issue a different operation span; the callback validates that span
    // against the active stream ring.
    static constexpr uint32_t kHalIoPeriodFrames = kV3ClientIoBudgetFrames;

    // The largest client IO AudioDriverKit lets a client pick at the V3 period
    // (min(zts * 3/8, 4096)). The IO budget is not enforced, so every TX
    // budget that must hold one whole write window is sized for this, not for
    // kHalIoPeriodFrames.
    static constexpr uint32_t kMaxClientIoFrames =
        AdkMaxClientIoFrames(kV3ZeroTimestampPeriodFrames1x);

    static constexpr uint32_t kFrameAlignment = 32;

    static constexpr uint32_t kRxDescriptorPackets = 504;

    // Packet-domain TX ownership (T5, documentation/TX_OWNERSHIP.md):
    //
    // - Hardware ring: the finite IT queue maps this many packets ahead of
    //   the newest completion. A refill later than the ring exhausts it and
    //   stops the stream, so the ring is the refill-stall budget (63 ms).
    // - Preparation slack: packets committed beyond the ring, so the refill
    //   finds every packet it maps committed even when the producer action
    //   is late. It is the producer-stall budget (63 ms).
    //
    // COVERAGE INVARIANT. A refill maps packets up to completion + ring and
    //   FATALs on one that is not committed. The producer therefore keeps
    //   committed packets up to completion + ring + slack (coverage). Since T4
    //   the producer arms only up to coverage; PCM reaches packets from the
    //   HAL ring through the fill, so no frame-exposure window is needed.
    static constexpr uint32_t kTxHardwareRingPackets = 504;
    // [TxPrep] telemetry buckets intentionally track the immutable hardware
    // floor rather than the larger, tuneable preparation lead.  That keeps a
    // captured distribution meaningful if the lead changes during tuning.
    static constexpr uint32_t kTxPreparationLatencyHistogramBuckets = 6;
    static constexpr uint32_t kTxCommittedMarginHistogramBuckets = 5;
    static constexpr uint32_t kRxCaptureOccupancyHistogramBuckets = 5;
    static constexpr uint64_t kNanosecondsPerMicrosecond = 1'000;
    static constexpr uint64_t kTxPreparationLatency250Us = 250;
    static constexpr uint64_t kTxPreparationLatency500Us = 500;
    static constexpr uint64_t kTxPreparationLatency750Us = 750;
    static constexpr uint64_t kTxPreparationLatency1000Us = 1'000;
    static constexpr uint64_t kTxPreparationLatency1500Us = 1'500;
    static constexpr uint32_t kTxCommittedMargin2xFloorPackets =
        2 * kTxHardwareRingPackets;
    static constexpr uint32_t kTxCommittedMargin4xFloorPackets =
        4 * kTxHardwareRingPackets;
    static constexpr uint32_t kTxCommittedMargin8xFloorPackets =
        8 * kTxHardwareRingPackets;
    static constexpr uint32_t kTxCommittedMargin16xFloorPackets =
        16 * kTxHardwareRingPackets;
    static constexpr uint32_t kTxPreparationSlackPackets =
        kTxHardwareRingPackets;
    static constexpr uint32_t kTxCoverageLeadPackets =
        kTxHardwareRingPackets + kTxPreparationSlackPackets;
    // TX fill finality guard (documentation/TX_OWNERSHIP.md, T4 and E2):
    // packets ahead of the projected hardware position that the fill still
    // treats as taken. 2 is what Saffire.kext writes ahead of the playhead;
    // with T5's descriptor-status completion the projection is exact at each
    // refill, and E2 ran clean with it at 64/32/16-frame buffers on the Pro 24
    // DSP (it was 3, from midi c3e27a53: ~2 packets of OHCI fetch + 1 slack).
    static constexpr uint32_t kTxFillFinalityGuardPackets = 2;
    // The producer's per-pass and total lead: coverage only (T5).
    static constexpr uint32_t kTxPreparationLeadPackets =
        kTxCoverageLeadPackets;
    // Backing ring: the preparation lead plus one hardware ring before a
    // slot is reused (1008 + 504 = 1512 packets, 189 ms).
    static constexpr uint32_t kTxSharedSlotPackets =
        kTxPreparationLeadPackets + kTxHardwareRingPackets;
    // Largest single coalesced deltaConsumed a refill can absorb without holing.
    static constexpr uint32_t kTxMaxCoveredDeltaConsumedPackets =
        kTxPreparationLeadPackets - kTxHardwareRingPackets;

    // Backing packet-ring / timeline slot array length
    // (AmdtpPacketTimeline, DiceTxStreamEngine::timelineSlots_). Packets.
    static constexpr uint32_t kTimelineSlots = kTxSharedSlotPackets;
};

static_assert(AudioTimingGeometry::kRxDescriptorPackets %
                  AudioTimingGeometry::kTimingGroupPackets ==
              0);
static_assert(AudioTimingGeometry::kRxDescriptorPackets %
                  AudioTimingGeometry::kCadenceBlockPackets ==
              0);
static_assert(AudioTimingGeometry::kTimingGroupPackets %
                  AudioTimingGeometry::kCadenceBlockPackets ==
              0,
              "A completion group must hold whole blocking cadence blocks so "
              "every group carries the same frame count (fixed phase)");
static_assert(AudioTimingGeometry::kNominalFramesPerTimingGroup == 48);

// --- V3 HAL geometry against the completion grid (decision D2) -------------
namespace detail {
[[nodiscard]] constexpr bool V3GeometryHoldsAt(uint32_t sampleRateHz) noexcept {
    const auto hal = HalBufferProfileForRate(sampleRateHz);
    const uint32_t tier = HalRateTier(sampleRateHz);
    const uint32_t groupFrames = AudioTimingGeometry::kNominalFramesPerTimingGroup * tier;
    const uint32_t blockFrames = AudioTimingGeometry::kCadenceBlockFrames * tier;
    return tier != 0 &&
           hal.frameRingFrames == hal.zeroTimestampPeriodFrames &&
           hal.frameRingFrames % hal.clientIoBudgetFrames == 0 &&
           hal.frameRingFrames % AudioTimingGeometry::kFrameAlignment == 0 &&
           hal.zeroTimestampPeriodFrames % blockFrames == 0 &&
           hal.zeroTimestampPeriodFrames % groupFrames == 0 &&
           hal.zeroTimestampPeriodFrames / groupFrames == 256 &&
           AdkMaxClientIoFrames(hal.zeroTimestampPeriodFrames) ==
               AudioTimingGeometry::kMaxClientIoFrames;
}
} // namespace detail
// 48/96/192 kHz: 512 cadence blocks, 256 eight-packet groups (2048 cycles,
// 256 ms) per ZTS period, ring == period, and the 4096-frame client ceiling.
static_assert(detail::V3GeometryHoldsAt(48'000));
static_assert(detail::V3GeometryHoldsAt(96'000));
static_assert(detail::V3GeometryHoldsAt(192'000));
static_assert(AudioTimingGeometry::kAllocatedFrameRingFrames %
                  HalBufferProfileForRate(48'000).frameRingFrames ==
              0,
              "a rate change must reuse the allocation, not resize it");
static_assert(AudioTimingGeometry::kMaxClientIoFrames == 4'096);
static_assert(AudioTimingGeometry::kTimingGroupPackets != 0,
              "Timing group packet count must be non-zero");
static_assert(AudioTimingGeometry::kRxPacketsPerGroup ==
                  AudioTimingGeometry::kTxPacketsPerGroup,
              "RX and TX interrupt groups must match");
static_assert(AudioTimingGeometry::kTxSharedSlotPackets >=
              AudioTimingGeometry::kTxPreparationLeadPackets,
              "TX shared slot ring must hold the full preparation lead");
// COVERAGE: tolerate 16 completion groups (128 packets, 16 ms) without a
// producer wake. This covers the observed 40-42 packet DriverKit dispatch
// stalls with more than 3x margin.
static_assert(AudioTimingGeometry::kTxMaxCoveredDeltaConsumedPackets >=
                  16 * AudioTimingGeometry::kTxPacketsPerGroup,
              "TX preparation headroom must cover at least 12 ms of producer "
              "dispatch latency at the current eight-packet cadence");
static_assert(AudioTimingGeometry::kTxCoverageLeadPackets ==
                  AudioTimingGeometry::kTxHardwareRingPackets +
                      AudioTimingGeometry::kTxPreparationSlackPackets,
              "TX coverage lead must remain the refill-safety sub-budget");
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
static_assert(AudioTimingGeometry::kTxSharedSlotPackets %
                  AudioTimingGeometry::kCadenceBlockPackets ==
              0,
              "TX shared slot wrap must preserve blocking-cadence phase");

static_assert(AudioTimingGeometry::kTxSharedSlotPackets <=
                  AudioTimingGeometry::kTimelineSlots,
              "shared packet ring must fit inside the timeline slot array");

// --- Finite IT queue coverage (T5). A refill maps packets up to
//     completion + ring and needs every one committed; the producer keeps
//     coverage beyond that, and a slot is reused only a ring after coverage.
//     AppleFWAudio's DCL ring is 100 groups of 8 packets (~100 ms, reference
//     block below), the same order as this 63 ms ring. -------------------
static_assert(AudioTimingGeometry::kTxCoverageLeadPackets >=
                  AudioTimingGeometry::kTxHardwareRingPackets +
                      AudioTimingGeometry::kTxPacketsPerGroup,
              "the producer must commit past what one refill can map");
static_assert(AudioTimingGeometry::kTxSharedSlotPackets >=
                  AudioTimingGeometry::kTxCoverageLeadPackets +
                      AudioTimingGeometry::kTxHardwareRingPackets,
              "a shared slot must not be re-armed while its packet is mapped");

} // namespace ASFW::IsochTransport

// =============================================================================
// REFERENCE GEOMETRY -- external stacks at 48 kHz blocking (AM824/IEC 61883-6)
//
// A cross-check ceiling for the constants above, gathered 2026-06-15 from the
// in-tree references. These are NOT requirements and NOT directly adopted --
// they are the closest analogues (names may differ) from stacks that are
// wire-correct against real hardware. Use them to sanity-check ours, never to
// override measured behavior. Full prose: documentation/ZTS_AND_SYT.md
// §9 (Linux/libffado) and §10 (Apple).
//
// Shared by all three AND us: 8000 cycles/s, 3072 ticks/cycle, 24'576'000
// ticks/s; blocking SYT interval (frames per DATA packet) = 8 @48k / 16 @96k /
// 32 @192k  (== our kFramesPerDataPacket and AmdtpRateGeometry::sytIntervalFrames).
//
// --- Apple AppleFWAudio.kext  (AM824DCLWrite / AM824NuDCLWrite, x86 IDA) ------
//   fNumBufferGroups          = 100        backing DCL ring depth (~100 ms)
//   fNumPacketsPerBufferGroup = 8          => HW interrupt every 8 pkt = 1.0 ms
//   48k cadence               = D,D,D,N    => 6 frames/cycle avg, 8-frame DATA
//   CheckSYT target latency   = 2-3 cycles (~250-375 us) device presentation
//                                          -- this is a SYT/presentation lead
//                                          (cf. our transfer delay/SYT),
//                                          NOT the CoreAudio safety offset.
//   servo update              = gated groupIndex==0 => ~100 ms / ring wrap
//                                          (100 groups * 8 pkt * 125 us)
//   our analogues: 8-pkt group == kTimingGroupPackets (8, 1.0 ms); 100*8=800-pkt
//     backing ring ~ kTxSharedSlotPackets / kTimelineSlots.
//   *** CAVEAT (load-bearing) ***  This is OS 9 / early-OS-X lineage code: the
//   buffer routines are literally the classic-Mac "DV" (Digital Video FireWire)
//   streaming path -- DVAllocatePlayBufferGroup / DVCreatePlayBufferGroupUpdate-
//   List, IOMallocAligned + fixed 100x8 rings, written ~2000-2003 and rarely
//   touched since. Its constants were tuned for that era's low-performance CPUs
//   and almost certainly for INTERRUPT-RATE / JITTER reduction (1 ms IRQ,
//   once-per-wrap servo to minimize interrupt-context work), not for 2026
//   latency. Take the ratios and the clock-domain discipline as the lesson, not
//   the absolute counts.
//
// --- Linux ALSA  sound/firewire/amdtp-stream.c -------------------------------
//   syt_interval @48k         = 8 frames/DATA packet
//   idle_irq_interval         = 6 pkt (0.75 ms) @ 32-frame period;
//                               11 pkt (1.375 ms) @ 64-frame period
//   minimum period            = 250 us = 2 pkt (hard floor)
//   transfer_delay base       = 0x2e00 ticks (11776); effective on-wire SYT lead
//                               re-added at encode vs the transmit cycle works
//                               out to ~12800 @48k (== our applied transfer delay).
//   Period-derived IRQ, close to the wire; no fixed deep lead-ahead.
//
// --- libffado-2.5.0  (IsoHandlerManager.cpp, util/cip.h, libieee1394/cycletimer.h)
//   syt_interval              = 8 / 16 / 32 @ 1x / 2x / 4x  (getSytInterval)
//   CIP_TRANSFER_DELAY        = 9000 ticks
//   MAX_XMIT/RECV_NB_BUFFERS  = 128         queue-depth ceiling
//   MINIMUM_INTERRUPTS_PER_PERIOD = 2
//   irq_interval = (packets_per_period-1)/min_interrupts, capped at buffers/2
//                  => ~4-8 pkt (0.5-1.0 ms) for a 64-frame period
//
// TAKEAWAYS for our geometry:
//   * Everyone services DMA far more often than a deep batch: 0.75-1.375 ms IRQ
//     (6-11 pkt). Ours kTimingGroupPackets = 8 (1.0 ms, Apple's value) is in range.
//   * Everyone keeps SYT/presentation in the 1394 tick domain, not host time.
//   * Backing ring depth (Apple ~800 pkt, ffado 128) is decoupled from the
//     active near-wire lead -- matches our capacity-is-not-latency rule. None of
//     these exposes a direct AudioDriverKit ZTS analogue (see §9.F), so the
//     ZTS period is OURS to justify, not inherited.
// =============================================================================
