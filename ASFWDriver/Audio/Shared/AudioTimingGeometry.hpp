#pragma once

#include "AudioHalBufferProfiles.hpp"
#include "../../Shared/Isoch/IsochQueueGeometry.hpp"

#include <cstdint>

namespace ASFW::Audio::Shared {

// AUDIO-ONLY CONTRACT. Do not include this header from Hardware/, Bus/,
// Async/, Isoch/, or Shared/: those layers carry opaque isoch packets and must
// not inherit audio cadence, frame, buffer, or clock policy. Generic producer /
// consumer queue geometry is imported through the neutral Shared/Isoch seam.

// -----------------------------------------------------------------------------
// UNIT DISCIPLINE: every constant carries its domain in the name --
//   ...Packets  FireWire isoch packets (one per 125 us cycle, 8000/s)
//   ...Frames   audio sample frames (48000/s at 1x)
//   ...Blocks   OHCI descriptor blocks (4 per TX packet, Z=4)
//   ...Ticks    24.576 MHz FireWire ticks (3072/cycle)
// Never compare a *Packets value to a *Frames value without an explicit
// conversion. V3 supports only the exact 48 kHz family (48/96/192 kHz).
//
// CURSOR MODEL (TX): the hardware timeline supplies every packet's absolute
// frame. WriteEnd only publishes immutable PCM for that coordinate. Missing
// content at the physical deadline becomes backend NO-DATA while hardware time
// advances; content is never retried late, clamped, or rebased.
//
// Rate-DEPENDENT geometry (safety offsets, frames-per-packet at 96/192 kHz,
// reported latency) lives in AudioGeometryPolicy.hpp, not here.
// -----------------------------------------------------------------------------
struct AudioTimingGeometry final {
    [[nodiscard]] static constexpr bool IsV3SampleRate(
        uint32_t sampleRateHz) noexcept {
        return sampleRateHz == 48'000 || sampleRateHz == 96'000 ||
               sampleRateHz == 192'000;
    }
    static constexpr uint32_t kSampleRateHz = 48000;

    // Blocking AMDTP cadence at 48k: D,D,D,N over 4 packets, 8 frames per
    // data packet => 24 frames per cadence block (6 frames/packet average).
    static constexpr uint32_t kFramesPerDataPacket = 8;
    static constexpr uint32_t kCadenceBlockPackets = 4;
    static constexpr uint32_t kCadenceBlockFrames = 24;

    // DMA completion cadence is deliberately independent from the HAL ZTS
    // grid. Six FireWire cycles give 0.75 ms refill latency. Depending on the
    // D/D/D/N starting phase, one interrupt carries 32 or 40 decoded frames.
    static constexpr uint32_t kRxPacketsPerGroup =
        ::ASFW::Shared::Isoch::IsochQueueGeometry::kPacketsPerCompletionGroup;
    static constexpr uint32_t kTxPacketsPerGroup =
        ::ASFW::Shared::Isoch::IsochQueueGeometry::kPacketsPerCompletionGroup;
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

    // Scheduling-jitter cushion (single Default-queue contention). Field runs
    // showed producer wakes delayed by tens of packets; every "must lead by"
    // budget adds this on top of its nominal requirement. Frames.
    static constexpr uint32_t kSchedulingJitterFrames = 64;

    // The graph applies the complete profile/output/client-IO formula. This is
    // only the interrupt-batch component of that calculation.
    static constexpr uint32_t kInputSafetyFloorFrames =
        kMaximumNominalFramesPerInterrupt + kSchedulingJitterFrames;

    // Client IO sizing/safety budget. ADK may issue a different operation
    // span; the callback validates that span against stream-ring capacity.
    static constexpr uint32_t kHalIoPeriodFrames =
        kActiveAudioHalBufferProfile.clientIoBudgetFrames;

    static constexpr uint32_t kFrameAlignment = 32;

    // Exactly one HAL revolution of immutable, epoch-keyed PCM. This is byte
    // retention only; it has no scheduling or latency meaning.
    static constexpr uint32_t kPcmPublicationCacheFrames = 8'192;

    // Packet-domain TX policy. The 48-slot ownership guard protects descriptors
    // already visible to OHCI. The 96-slot dispatch allowance keeps a measured
    // scheduling stall from emptying the prepared queue. Their sum is the V3
    // physical-time preparation target; neither value exposes PCM or changes
    // the absolute sample timeline.
    static constexpr uint32_t kTxHardwareRingPackets =
        ::ASFW::Shared::Isoch::IsochQueueGeometry::kTransmitInFlightPackets;
    // [TxPrep] telemetry buckets intentionally track the immutable hardware
    // floor rather than the larger, tuneable preparation lead.  That keeps a
    // captured distribution meaningful if the lead changes during tuning.
    static constexpr uint32_t kTxPreparationLatencyHistogramBuckets = 6;
    static constexpr uint32_t kTxCommittedMarginHistogramBuckets = 5;
    static constexpr uint32_t kTxDeadlineHeadroomHistogramBuckets = 5;
    static constexpr uint32_t kTxCompletionLatencyHistogramBuckets = 5;
    static constexpr uint32_t kRxCaptureOccupancyHistogramBuckets = 5;
    static constexpr uint64_t kNanosecondsPerMicrosecond = 1'000;
    static constexpr uint64_t kTxPreparationLatency250Us = 250;
    static constexpr uint64_t kTxPreparationLatency500Us = 500;
    static constexpr uint64_t kTxPreparationLatency750Us = 750;
    static constexpr uint64_t kTxPreparationLatency1000Us = 1'000;
    static constexpr uint64_t kTxPreparationLatency1500Us = 1'500;
    // Committed-margin histogram ceilings, in packets. The bucket that matters
    // is "how close did we come to holing the descriptor ring", so the ladder
    // resolves fractions of the hardware ring, not multiples of it: the shared
    // store is 168 packets, so a 2x/4x/8x/16x-ring ladder put every sample in
    // one bucket and resolved nothing. Overrunning the ring is a transport
    // failure (IT FATAL: slot not committed), not a recoverable content gap,
    // which is why the resolution belongs at the low end.
    static constexpr uint32_t kTxCommittedMarginQuarterRingPackets =
        kTxHardwareRingPackets / 4;
    static constexpr uint32_t kTxCommittedMarginHalfRingPackets =
        kTxHardwareRingPackets / 2;
    static constexpr uint32_t kTxCommittedMarginThreeQuarterRingPackets =
        (kTxHardwareRingPackets * 3) / 4;
    static constexpr uint32_t kTxCommittedMarginOneRingPackets =
        kTxHardwareRingPackets;
    // Refill-latency budget: how late the producer may be and still find its
    // slots committed. This is a property of scheduling jitter, NOT of ring
    // depth — a deeper hardware ring buys runway, it does not require more
    // look-ahead.
    //
    // It was previously written as `2 * kTxHardwareRingPackets`, which made the
    // required look-ahead scale with the ring: kTxCoverageLeadPackets = 3 * ring.
    // Deepening the ring then *raised* the amount of future the producer had to
    // materialise, and preparation is fed by RX replay observations that are
    // inherently ~now, so it could not. asfw_sim bisected the ceiling at ring
    // 312 (39 ms); at ring 800 the producer would have had to run 117 ms ahead
    // of hardware and the stream collapsed to 0% written — with every
    // static_assert still passing. See
    // tools/asfw_sim/scenarios/slack-scales-with-ring.yaml.
    //
    // Was 96 (12 ms) -- the value the old ring-scaled expression produced at the
    // shipping ring of 48. Reduced to 72 (9 ms) on 2026-08-29, because the slack
    // is carried as *committed* DMA lead and therefore lands in the CoreAudio
    // output safety offset: at 96 the Duet reported 928 safety frames, i.e.
    // 24.7 ms of output latency in Logic, of which 18 ms was this budget.
    //
    // 72 still covers the observed 40-42 packet DriverKit dispatch stalls with
    // ~1.7x margin (see the COVERAGE assert below, relaxed from 16 groups to 12
    // in the same change). It is not taken lower: at 48 the margin over those
    // stalls would be 1.14x, and overrunning this budget holes the descriptor
    // ring, which silence substitution does not rescue -- that is a transport
    // failure (IT FATAL: slot not committed), not a content gap.
    static constexpr uint32_t kTxOwnershipGuardCycleSlots =
        kTxHardwareRingPackets;
    static constexpr uint32_t kTxDispatchSlackCycleSlots = 72;
    static constexpr uint32_t kTxPreparedTargetCycleSlots =
        kTxOwnershipGuardCycleSlots + kTxDispatchSlackCycleSlots;
    // Compatibility spelling while the remaining call sites are migrated to
    // physical presentation plans.
    static constexpr uint32_t kTxPreparationSlackPackets =
        kTxDispatchSlackCycleSlots;
    static constexpr uint32_t kTxCoverageLeadPackets =
        kTxPreparedTargetCycleSlots;
    static constexpr uint32_t kTxPreparationLeadPackets =
        kTxPreparedTargetCycleSlots;
    // 21 ms of durable packet storage: 15 ms prepared plus the 6 ms ownership
    // guard. Storage capacity is not presentation latency.
    // Freeze frontier: how far ahead of the hardware a packet's samples stop
    // being writable. Transport binds a slot to a descriptor at most one
    // hardware ring ahead, and advances that frontier one completion group at a
    // time, so a fill must land this far ahead to be certain of winning.
    //
    // This is the ONLY TX depth that becomes CoreAudio output latency. The arm
    // horizon above may grow freely to absorb scheduling stalls: an armed
    // packet already holds a valid silent image, so a late producer costs
    // content, never a holed descriptor ring.
    static constexpr uint32_t kTxContentFreezeCycleSlots =
        kTxHardwareRingPackets + kTxPacketsPerGroup;
    static constexpr uint32_t kTxSharedSlotPackets = 168;
    // Largest single coalesced deltaConsumed a refill can absorb without holing.
    static constexpr uint32_t kTxMaxCoveredDeltaConsumedPackets =
        kTxPreparedTargetCycleSlots - kTxOwnershipGuardCycleSlots;

    // Backing packet-ring / timeline slot array length
    // (AmdtpPacketTimeline, DiceTxStreamEngine::timelineSlots_). Packets.
    static constexpr uint32_t kTimelineSlots = kTxSharedSlotPackets;
};

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
// COVERAGE: tolerate 12 six-packet groups without a producer wake, ~1.7x the
// observed 40-42 packet DriverKit dispatch stalls. Relaxed from 16 groups when
// the dispatch slack went 96 -> 72; the margin bought latency, since this budget
// is committed lead and is reported to CoreAudio as output safety. Do not take
// it below 12: overrunning it holes the descriptor ring, and unlike a PCM gap
// that is a transport failure silence substitution cannot cover.
static_assert(AudioTimingGeometry::kTxMaxCoveredDeltaConsumedPackets >=
                  12 * AudioTimingGeometry::kTxPacketsPerGroup,
              "TX preparation headroom must cover at least 9 ms of producer "
              "dispatch latency at the current six-packet cadence");
static_assert(AudioTimingGeometry::kTxCoverageLeadPackets ==
                  AudioTimingGeometry::kTxHardwareRingPackets +
                      AudioTimingGeometry::kTxPreparationSlackPackets,
              "TX coverage lead must remain the refill-safety sub-budget");
static_assert(AudioTimingGeometry::kTxSharedSlotPackets ==
                  AudioTimingGeometry::kTxPreparedTargetCycleSlots +
                      AudioTimingGeometry::kTxOwnershipGuardCycleSlots,
              "TX store must hold prepared slots plus the ownership guard");
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

} // namespace ASFW::Audio::Shared

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
// 32 @192k  (== our kFramesPerDataPacket and AudioGeometryPolicy::FramesPerPacket).
//
// --- Apple AppleFWAudio.kext  (AM824DCLWrite / AM824NuDCLWrite, x86 IDA) ------
// Full reference: documentation/APPLE_FWAUDIO_ISOCH_GEOMETRY.md (2026-08-26).
// NOTE the binary carries TWO lineages; they differ by 100x in IRQ rate, so
// always say which one you mean.
//   fNumBufferGroups          = 100        backing DCL ring depth (~100 ms)
//   fNumPacketsPerBufferGroup = 8          bookkeeping chunk, NOT the IRQ stride
//   kCallbackTimeoutInMSec    = 20
//   legacy DCL (non-blocking): CallProc per group => IRQ every  8 pkt (1000/s)
//   NuDCL RX   (blocking):     every 20th group   => IRQ every 160 pkt (  50/s)
//   NuDCL TX   (blocking):     once per ring lap  => IRQ every 800 pkt (  10/s)
//     NuDCL RX divisor is derived from a TIME budget, not a packet count:
//     buffersPerCallback = 20000us / (125us * packetsPerGroup) = 20.
//   Per-packet timing survives the coarse IRQ because every packet carries
//   setTimeStampPtr -- DMA writes the timestamp, no interrupt needed to read
//   it. Apple answers "is there work?" and "what time is it?" with two
//   different mechanisms; we answer both with the completion interrupt.
//   TX refill is driven by the CoreAudio clip path + an IOTimerEventSource
//   (AppleFWAudioIsocEngine::scheduleDelayedWork), so a lost isoch interrupt
//   costs a lap marker, not the stream. See linux-apple-irq-silence-guards.
//   48k cadence (legacy)      = D,D,D,N    => 6 frames/cycle avg, 8-frame DATA
//   CheckSYT target latency   = 2-3 cycles (~250-375 us) device presentation
//                                          -- this is a SYT/presentation lead
//                                          (cf. our TxTransferDelayTicks/SYT),
//                                          NOT the CoreAudio safety offset.
//   servo update              = gated groupIndex==0 => ~100 ms / ring wrap
//                                          (100 groups * 8 pkt * 125 us)
//   our analogues: Apple's 8-pkt group is a BOOKKEEPING chunk and has no
//     analogue here -- it exists to precompute a DCLUpdateDCLList and a
//     callback refcon so the ISR does no pointer-chasing through a linked
//     list. Our descriptors are a flat array (index % ringSize), so all three
//     of its jobs are free. Do NOT port buffer groups. What IS transferable:
//     Apple keeps chunk size (8) and IRQ stride (160/800) as DIFFERENT
//     numbers, where kPacketsPerCompletionGroup fuses IRQ stride, ZTS timing
//     stride and refill quantum into one constant.
//   100*8=800-pkt backing ring ~ kTxSharedSlotPackets / kTimelineSlots.
//   *** CAVEAT (load-bearing) ***  The LEGACY DCL path is OS 9 / early-OS-X
//   lineage code: the buffer routines are literally the classic-Mac "DV"
//   (Digital Video FireWire) streaming path -- DVAllocatePlayBufferGroup /
//   DVCreatePlayBufferGroupUpdateList, IOMallocAligned + fixed 100x8 rings,
//   written ~2000-2003 and rarely touched since. Its 1 ms IRQ was tuned for
//   that era's CPUs. The NuDCL path is the later one and is the relevant
//   comparison for us, because it is BLOCKING like ours (legacy is
//   non-blocking 5/6). Take the ratios and the clock-domain discipline as the
//   lesson, not the absolute counts.
//
// --- Linux ALSA  sound/firewire/amdtp-stream.c -------------------------------
//   syt_interval @48k         = 8 frames/DATA packet
//   idle_irq_interval         = 6 pkt (0.75 ms) @ 32-frame period;
//                               11 pkt (1.375 ms) @ 64-frame period
//   minimum period            = 250 us = 2 pkt (hard floor)
//   transfer_delay base       = 0x2e00 ticks (11776); effective on-wire SYT lead
//                               re-added at encode vs the transmit cycle works
//                               out to ~12800 @48k (== our TxTransferDelayTicks).
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
//   * IRQ cadence is NOT converged across stacks, and the earlier claim here
//     that "everyone services DMA every 0.75-1.375 ms, ours is in range" was
//     wrong: it generalised from Apple's LEGACY path only. Actual spread is
//     10/s (Apple NuDCL TX) .. 50/s (Apple NuDCL RX) .. ~1000-1300/s (legacy
//     Apple, Linux, ffado) .. 1333/s (ours). The two stacks closest to our
//     model -- blocking AMDTP -- are the two slowest ones.
//   * What the fast stacks have that we lack is a NON-IRQ path into the same
//     completion code (Linux flush_completions from the PCM period; Apple's
//     work timer). Cadence is a consequence of that, not the design choice.
//   * Everyone keeps SYT/presentation in the 1394 tick domain, not host time.
//   * Backing ring depth (Apple ~800 pkt, ffado 128) is decoupled from the
//     active near-wire lead -- matches our capacity-is-not-latency rule. None of
//     these exposes a direct AudioDriverKit ZTS analogue (see §9.F), so
//     the ZTS period is ours to justify, not inherited.
// =============================================================================
