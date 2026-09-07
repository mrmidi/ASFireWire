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

    // The isochronous cycle grid. Duplicated from ASFW::Timing (Common/
    // TimingUtils.hpp) rather than included, because that header pulls in
    // DriverKit/IOLib.h and this one is a pure constants header the host tests
    // compile on its own. The static_asserts below pin the relationship.
    static constexpr uint32_t kIsochCyclesPerSecond = 8'000;
    static constexpr uint32_t kMicrosecondsPerIsochCycle = 125;

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

    // Scheduling-jitter cushion used for immutable publication retention. It
    // is not a HAL safety-offset floor; device profiles own reported safety.
    static constexpr uint32_t kSchedulingJitterFrames = 64;

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
    static constexpr uint32_t kRxHardwareRingPackets =
        ::ASFW::Shared::Isoch::IsochQueueGeometry::kReceiveInFlightPackets;
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
    // store is three rings deep, so a 2x/4x/8x/16x-ring ladder put every sample
    // in one bucket and resolved nothing. Overrunning the ring is a transport
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
    // Was 96 (12 ms), then 72 (9 ms) on 2026-08-29, on the grounds that the
    // slack was carried as committed DMA lead and therefore landed in the
    // CoreAudio output safety offset. That reasoning is now obsolete: payload
    // finality was decoupled from descriptor binding, and
    // AudioGeometryPolicy::RequiredOutputSafetyFrames derives safety from
    // kTxContentFreezeCycleSlots (8 packets) alone. Nothing outside this header
    // reads the slack except the tuning default and the geometry report, so
    // widening it costs memory and nothing else.
    //
    // Raised to 504 on 2026-09-07 alongside the ring, because the two are not
    // independent, however much the paragraph above wants them to be:
    //
    //   the mapped window is kTxHardwareRingPackets deep and one completion
    //   anchor is retained, so a single refill observes at most ring - 1
    //   finished packets -- and must find every one of them committed.
    //
    // Hence the COVERAGE assert below: slack >= ring - 1. Survival is therefore
    // min(ring, slack + 1), and raising the ring alone would only relocate the
    // fatal from MappedRegionExhausted to "IT FATAL: slot not committed" one
    // packet later. This is NOT the old `2 * ring` policy that made the lead
    // 3 * ring and collapsed the producer: the requirement is exactly ring - 1,
    // so the lead is 2 * ring, and preparation arms from silence rather than
    // from content it cannot have yet.
    //
    // A second, independent floor argues the same way: the producer's other
    // wake source is the CoreAudio IO callback at kHalIoPeriodFrames (512
    // frames = 10.67 ms at 48 kHz). The old 72 packets was 9.0 ms -- less than
    // one callback period, so a producer woken only by CoreAudio was already
    // structurally late.
    static constexpr uint32_t kTxOwnershipGuardCycleSlots =
        kTxHardwareRingPackets;
    static constexpr uint32_t kTxDispatchSlackCycleSlots = 504;
    // The absolute-time floor the COVERAGE assert below enforces, named once so
    // validation and the tuning panel quote the same number instead of each
    // spelling out twelve groups of six. This is the operator-facing floor: it
    // bounds how far the panel may wind the slack DOWN, and it is a scheduling
    // budget in milliseconds, independent of ring depth. The structural
    // requirement (slack >= ring - 1) is a separate assert and is the larger of
    // the two at the shipping ring.
    static constexpr uint32_t kTxDispatchSlackFloorGroups = 12;
    static constexpr uint32_t kTxDispatchSlackAbsoluteTimeFloorPackets =
        kTxDispatchSlackFloorGroups * kTxPacketsPerGroup;
    // The structural floor: one refill can observe a full mapped window of
    // finished packets and must find every one committed, so slack below the
    // ring cannot cover the stall the ring exists to survive. Expressed as the
    // whole ring rather than ring - 1 so the panel can still label it in whole
    // completion groups; the exact bound is the static_assert below.
    static constexpr uint32_t kTxDispatchSlackRingCoverageFloorPackets =
        kTxHardwareRingPackets;
    // What the tuning panel is told the floor is. It must be the binding one of
    // the two, or the panel offers presets it describes as safe and which
    // deterministically hole the descriptor ring. Before the ring went to RX
    // parity the absolute-time floor was the larger; now the ring is.
    static constexpr uint32_t kTxDispatchSlackFloorPackets =
        kTxDispatchSlackAbsoluteTimeFloorPackets >
                kTxDispatchSlackRingCoverageFloorPackets
            ? kTxDispatchSlackAbsoluteTimeFloorPackets
            : kTxDispatchSlackRingCoverageFloorPackets;
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
    // 189 ms of durable packet storage: 126 ms prepared plus the 63 ms
    // ownership guard. Storage capacity is not presentation latency.
    // Bound-payload finality: transport refreshes alternative payload images on
    // each six-packet completion and never repoints either of the two commands
    // closest to the live OHCI CommandPtr. The next completion interval plus
    // that guard is therefore the producer-visible finality frontier.
    //
    // This is the ONLY TX depth that becomes CoreAudio output latency. The arm
    // horizon above may grow freely to absorb scheduling stalls: an armed
    // packet already holds a valid silent image, so a late producer costs
    // content, never a holed descriptor ring.
    static constexpr uint32_t kTxDescriptorRepointGuardCycleSlots =
        ::ASFW::Shared::Isoch::IsochQueueGeometry::
            kPayloadRepointGuardPackets;
    static constexpr uint32_t kTxContentFreezeCycleSlots =
        ::ASFW::Shared::Isoch::IsochQueueGeometry::
            kPayloadFinalityLeadPackets;
    // Cross-process payload/metadata slot count, stated explicitly rather than
    // derived because it sizes shared memory; the STORE assert below pins it to
    // prepared target + ownership guard = 3 * ring.
    static constexpr uint32_t kTxSharedSlotPackets = 1512;
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
static_assert(AudioTimingGeometry::kIsochCyclesPerSecond *
                  AudioTimingGeometry::kMicrosecondsPerIsochCycle ==
              1'000'000,
              "Isoch cycle grid must be 8000 cycles of 125 us per second");
static_assert(AudioTimingGeometry::kSampleRateHz %
                  AudioTimingGeometry::kIsochCyclesPerSecond ==
              0,
              "V3 rates must divide the cycle grid so frames-per-cycle is exact");
static_assert(AudioTimingGeometry::kRxHardwareRingPackets %
                  AudioTimingGeometry::kRxPacketsPerGroup ==
              0,
              "RX hardware ring must be an integer number of groups");
static_assert(AudioTimingGeometry::kRxPacketsPerGroup ==
                  AudioTimingGeometry::kTxPacketsPerGroup,
              "RX and TX interrupt groups must match");
static_assert(AudioTimingGeometry::kTxSharedSlotPackets >=
              AudioTimingGeometry::kTxPreparationLeadPackets,
              "TX shared slot ring must hold the full preparation lead");
// COVERAGE (structural): a refill walks the descriptors the consumer finished
// since the last pass and requires every one of them to be committed. The
// mapped window is one hardware ring deep and one completed descriptor is
// retained as the resume anchor, so a single pass can observe at most
// ring - 1 finished packets. Slack below that lets a stall the ring was sized
// to survive fail one packet later as "IT FATAL: slot not committed" instead of
// MappedRegionExhausted -- a relocation of the fatal, not a fix. This is what
// makes ring depth and slack a single decision; see kTxDispatchSlackCycleSlots.
static_assert(AudioTimingGeometry::kTxMaxCoveredDeltaConsumedPackets + 1 >=
                  AudioTimingGeometry::kTxHardwareRingPackets,
              "TX dispatch slack must cover a full mapped window of coalesced "
              "completions (ring - 1), or the deeper ring buys no survival");

// COVERAGE (absolute time): independently of ring depth, tolerate 12 six-packet
// groups without a producer wake -- ~1.7x the observed 40-42 packet DriverKit
// dispatch stalls, and more than one CoreAudio IO callback period, which is the
// producer's other wake source. Overrunning it holes the descriptor ring, and
// unlike a PCM gap that is a transport failure silence substitution cannot
// cover.
static_assert(AudioTimingGeometry::kTxMaxCoveredDeltaConsumedPackets >=
                  AudioTimingGeometry::kTxDispatchSlackAbsoluteTimeFloorPackets,
              "TX preparation headroom must cover at least 9 ms of producer "
              "dispatch latency at the current six-packet cadence");

// The floor published to the tuning panel must be the binding one, so a preset
// the panel does not mark "below floor" is actually coverable.
static_assert(AudioTimingGeometry::kTxDispatchSlackFloorPackets >=
                  AudioTimingGeometry::kTxDispatchSlackAbsoluteTimeFloorPackets &&
              AudioTimingGeometry::kTxDispatchSlackFloorPackets >=
                  AudioTimingGeometry::kTxDispatchSlackRingCoverageFloorPackets,
              "the published slack floor must dominate every floor that binds");
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
