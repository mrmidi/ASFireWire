#pragma once

#include "AudioHalBufferProfiles.hpp"

#include <algorithm>
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
// 5.5125 at 44.1k -- budgets use the worst case, kMinAvgCadence*).
//
// CURSOR MODEL (TX): three frame-domain cursors race --
//   T hardware transmit pos,  W CoreAudio write frontier,  E exposure frontier.
// A PCM frame survives iff  T <= W <= E. The only failure is W > E
// (under-exposure = `withoutPkt` = Defect B). The cushion that prevents it is
// kTxExposureLeadFrames below. See documentation/ZTS_AND_SYT.md and
// tools/tx_payload_ownership_sim.py. Live evidence of W > E: [TxPrepFrame].
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

    // Worst-case average cadence across supported 1x rates: the 44.1k family
    // carries 441 frames per 80 packets (5.5125 frames/packet average), fewer
    // than 48k's 6. Packet budgets that must cover a frame requirement are
    // sized with this ratio so they hold at every supported rate
    // (tools/amdtp_blocking_cadence_sim.py, production wiring item 4).
    static constexpr uint32_t kMinAvgCadencePackets = 80;
    static constexpr uint32_t kMinAvgCadenceFrames = 441;

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

    // Scheduling-jitter cushion (single Default-queue contention). Field runs
    // showed producer wakes delayed by tens of packets; every "must lead by"
    // budget adds this on top of its nominal requirement. Frames.
    static constexpr uint32_t kSchedulingJitterFrames = 64;

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

    // === TX exposure lead (E - W) -- the audio-frame cushion ================
    // Minimum audio frames the producer's exposure frontier (E) must keep ahead
    // of CoreAudio's write-window end. TX analogue of RX's
    // input-safety cushion (ResolveInputSafetyFrames): RX had it, TX did not -- which is why
    // TX shipped silence when the writer ran beyond ExposedFrameEnd()
    // (Defect B = under-exposure, W > E). Conservative form = one full
    // AppleFWAudio's AM824NuDCLWrite keeps its CIP insertion target roughly
    // 400 FireWire cycles ahead of the client write frontier at 48 kHz.  This
    // is content lead, not the much smaller OHCI descriptor/refill lead. Keep
    // the invariant in packet time so it remains a 50 ms horizon at every 1x
    // sample rate (the runtime converts it to frames for the active stream).
    // See AVC_RECOVERY_AND_SYNC_ALGO_AND_BUGS.md, "Apple reference target".
    //
    // Floor: one whole maximum client write window plus jitter. One WriteEnd
    // advances W by the client's IO size at once, before the producer can run,
    // so a horizon shorter than the IO leaves the tail of every write without
    // a packet (Defect B). With V3 clients may pick 4096 frames, above the
    // 2400-frame 400-cycle horizon at 48 kHz: tools/tx_data_horizon_burst_sim.py
    // run --io-frames 4096 drops 1696 frames per write without the floor.
    static constexpr uint32_t kTxDataHorizonPackets = 400;
    static constexpr uint32_t kTxExposureFloorFrames =
        kMaxClientIoFrames + kSchedulingJitterFrames; // 4,160

    [[nodiscard]] static constexpr uint32_t TxDataHorizonFrames(
        uint32_t sampleRateHz) noexcept {
        return std::max((kTxDataHorizonPackets * sampleRateHz + 7999) / 8000,
                        kTxExposureFloorFrames);
    }
    // The 1x cushion (the floor wins at 32/44.1/48 kHz): 4,160 frames.
    // (Spelled out: a static member function is not usable in a constant
    // expression inside its own class; a static_assert below pins equality.)
    static constexpr uint32_t kTxExposureLeadFrames =
        std::max((kTxDataHorizonPackets * kSampleRateHz + 7999) / 8000,
                 kTxExposureFloorFrames);
    // Packet lead deep enough to expose that many frames at the worst-case
    // (44.1k) average cadence: ceil(4160 / 5.5125) = 755 packets, rounded up
    // to a whole interrupt group (760) so every budget derived from it keeps
    // the group- and cadence-block-aligned ring-wrap asserts below.
    static constexpr uint32_t kTxExposureLeadPacketsRaw =
        (kTxExposureLeadFrames * kMinAvgCadencePackets +
         kMinAvgCadenceFrames - 1) /
        kMinAvgCadenceFrames;
    static constexpr uint32_t kTxExposureLeadPackets =
        ((kTxExposureLeadPacketsRaw + kTxPacketsPerGroup - 1) /
         kTxPacketsPerGroup) *
        kTxPacketsPerGroup;

    // Packet-domain TX ownership: 48 descriptors on hardware, plus two
    // independent producer budgets:
    //
    // 1. Refill coverage: packets that keep the core from holing the OHCI
    //    refill when the producer action is delayed.
    // 2. Frame exposure: extra packets the producer may prepare so the AMDTP
    //    timeline covers CoreAudio's latest WriteEnd plus kTxExposureLeadFrames.
    //
    // COVERAGE INVARIANT (hardware-confirmed). The IT refill ISR checks slots
    //   [completion + hardwareRing, completion + hardwareRing + deltaConsumed)
    // and FATALs if any is not yet committed. The coverage target stays
    // hardwareRing + slack (144 packets), but the total preparation limit is
    // larger so the audio-frame invariant can be satisfied without reusing
    // hardware-owned shared slots.
    static constexpr uint32_t kTxHardwareRingPackets = 48;
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
        2 * kTxHardwareRingPackets;
    static constexpr uint32_t kTxCoverageLeadPackets =
        kTxHardwareRingPackets + kTxPreparationSlackPackets;
    // Covers a full client write window plus the output exposure cushion when
    // the producer target is expressed as WriteEnd + kTxExposureLeadFrames.
    // The producer needs to preserve a whole maximum CoreAudio write window
    // in addition to the data horizon. Round the result to an interrupt
    // group: ceil((4096 + 4160) / 5.5125) = 1498 -> 1504 packets.
    static constexpr uint32_t kTxFrameExposureWindowPacketsRaw =
        ((kMaxClientIoFrames + kTxExposureLeadFrames) *
             kMinAvgCadencePackets +
         kMinAvgCadenceFrames - 1) /
        kMinAvgCadenceFrames;
    static constexpr uint32_t kTxFrameExposureWindowPackets =
        ((kTxFrameExposureWindowPacketsRaw + kTxPacketsPerGroup - 1) /
         kTxPacketsPerGroup) *
        kTxPacketsPerGroup;
    static constexpr uint32_t kTxPreparationLeadPackets =
        kTxCoverageLeadPackets + kTxFrameExposureWindowPackets;
    // Backing ring: the preparation lead plus one OHCI ring depth before a
    // slot is reused (1648 + 48 = 1696 packets, 212 ms). It also keeps the
    // exposure lead below half the ring. The 48-packet hardware descriptor
    // ring remains a separate low-latency transport concern (the 504-packet
    // ring and late binding are Epic 6, FW-209).
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

// --- TX exposure cushion (Defect B guard: the invariant whose absence let TX
//     ship ~1% silence). E must lead W by a full IO window plus jitter, for
//     the largest window a client can pick, at every rate. ------------------
static_assert(AudioTimingGeometry::kTxExposureLeadFrames >=
                  AudioTimingGeometry::kMaxClientIoFrames +
                      AudioTimingGeometry::kSchedulingJitterFrames,
              "TX exposure lead must cover one full IO window plus scheduling "
              "jitter (the cushion whose absence was Defect B)");
static_assert(AudioTimingGeometry::kTxExposureLeadFrames ==
              AudioTimingGeometry::TxDataHorizonFrames(AudioTimingGeometry::kSampleRateHz));
static_assert(AudioTimingGeometry::TxDataHorizonFrames(44'100) >=
              AudioTimingGeometry::kTxExposureFloorFrames);
static_assert(AudioTimingGeometry::TxDataHorizonFrames(96'000) >=
              AudioTimingGeometry::kTxExposureFloorFrames);
// 2x/4x rates carry 12/24 frames per packet on average, so the 1x packet
// window over-covers their larger horizon.
static_assert(AudioTimingGeometry::kTxFrameExposureWindowPackets * 12 >=
              AudioTimingGeometry::kMaxClientIoFrames +
                  AudioTimingGeometry::TxDataHorizonFrames(96'000));
static_assert(AudioTimingGeometry::kTxFrameExposureWindowPackets * 24 >=
              AudioTimingGeometry::kMaxClientIoFrames +
                  AudioTimingGeometry::TxDataHorizonFrames(192'000));
static_assert(AudioTimingGeometry::kTxExposureLeadPackets <=
                  AudioTimingGeometry::kTxSharedSlotPackets,
              "TX packet lead must be able to hold the required exposure frames");
static_assert(AudioTimingGeometry::kTxSharedSlotPackets >=
                  2 * AudioTimingGeometry::kTxExposureLeadPackets,
              "TX backing ring must keep the exposure target below half ring");
static_assert(AudioTimingGeometry::kTxFrameExposureWindowPackets *
                  AudioTimingGeometry::kMinAvgCadenceFrames >=
              (AudioTimingGeometry::kMaxClientIoFrames +
               AudioTimingGeometry::kTxExposureLeadFrames) *
                  AudioTimingGeometry::kMinAvgCadencePackets,
              "TX frame-exposure packet window must cover WriteEnd plus the "
              "exposure cushion at the worst-case (44.1k) cadence");
static_assert(AudioTimingGeometry::kTxSharedSlotPackets <=
                  AudioTimingGeometry::kTimelineSlots,
              "shared packet ring must fit inside the timeline slot array");

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
//     these exposes a direct AudioDriverKit ZTS analogue (see §9.F), so
//     kTxExposureLeadFrames and the ZTS period are OURS to justify, not inherited.
// =============================================================================
