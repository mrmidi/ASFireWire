#pragma once

#include "AudioHalBufferProfiles.hpp"

#include <cstdint>

namespace ASFW::IsochTransport {

// -----------------------------------------------------------------------------
// UNIT DISCIPLINE: every constant carries its domain in the name --
//   ...Packets  FireWire isoch packets (one per 125 us cycle, 8000/s)
//   ...Frames   audio sample frames (48000/s at 1x)
//   ...Blocks   OHCI descriptor blocks (4 per TX packet, Z=4)
//   ...Ticks    24.576 MHz FireWire ticks (3072/cycle)
// Never compare a *Packets value to a *Frames value without an explicit
// conversion (the cadence is the only bridge: 6 frames/packet average).
//
// CURSOR MODEL (TX): three frame-domain cursors race --
//   T hardware transmit pos,  W CoreAudio write frontier,  E exposure frontier.
// A PCM frame survives iff  T <= W <= E. The only failure is W > E
// (under-exposure = `withoutPkt` = Defect B). The cushion that prevents it is
// kTxExposureLeadFrames below. See documentation/ZTS_AND_SYT.md and
// tools/tx_payload_ownership_sim.py / tools/analyze_payloadwriter.py.
//
// Rate-DEPENDENT geometry (safety offsets, frames-per-packet at 96/192 kHz,
// reported latency) lives in AudioGeometryPolicy.hpp, not here.
// -----------------------------------------------------------------------------
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

    static constexpr uint32_t kRxDescriptorPackets = 504;

    // === TX exposure lead (E - W) -- the audio-frame cushion ================
    // Minimum audio frames the producer's exposure frontier (E) must keep ahead
    // of CoreAudio's write-window end. TX analogue of RX's
    // RequiredInputSafetyFrames cushion: RX had it, TX did not -- which is why
    // TX shipped silence when the writer ran beyond ExposedFrameEnd()
    // (Defect B = under-exposure, W > E). Conservative form = one full
    // client-IO budget + scheduling jitter; tighten toward the actual IO size
    // once measured. Frames.
    static constexpr uint32_t kTxExposureLeadFrames =
        kHalIoPeriodFrames + kSchedulingJitterFrames;          // 512 + 64 = 576
    // Packet lead deep enough to expose that many frames at the six-frame
    // average cadence. ceil(576 / 6) = 96 packets.
    static constexpr uint32_t kTxExposureLeadPackets =
        (kTxExposureLeadFrames * kCadenceBlockPackets +
         kCadenceBlockFrames - 1) /
        kCadenceBlockFrames;

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
    static constexpr uint32_t kTxPreparationSlackPackets =
        2 * kTxHardwareRingPackets;
    static constexpr uint32_t kTxCoverageLeadPackets =
        kTxHardwareRingPackets + kTxPreparationSlackPackets;
    // Covers a full client write window plus the output exposure cushion when
    // the producer target is expressed as WriteEnd + kTxExposureLeadFrames.
    // 2 * 96 packets = 1152 nominal frames, enough for 512 + 576 = 1088 frames
    // while preserving cadence/group-friendly packet counts.
    static constexpr uint32_t kTxFrameExposureWindowPackets =
        2 * kTxExposureLeadPackets;
    static constexpr uint32_t kTxPreparationLeadPackets =
        kTxCoverageLeadPackets + kTxFrameExposureWindowPackets;
    // Shared backing leaves one hardware-ring depth before slot reuse.
    static constexpr uint32_t kTxSharedSlotPackets =
        kTxPreparationLeadPackets + kTxHardwareRingPackets;
    // Largest single coalesced deltaConsumed a refill can absorb without holing.
    static constexpr uint32_t kTxMaxCoveredDeltaConsumedPackets =
        kTxPreparationLeadPackets - kTxHardwareRingPackets;

    // Backing packet-ring / timeline slot array length
    // (AmdtpPacketTimeline, DiceTxStreamEngine::timelineSlots_). Packets.
    static constexpr uint32_t kTimelineSlots = 512;
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
              "TX preparation headroom must cover at least 12 ms of producer "
              "dispatch latency at the current six-packet cadence");
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
//     ship ~1% silence). E must lead W by a full IO window plus jitter. -------
static_assert(AudioTimingGeometry::kTxExposureLeadFrames >=
                  AudioTimingGeometry::kHalIoPeriodFrames +
                      AudioTimingGeometry::kSchedulingJitterFrames,
              "TX exposure lead must cover one full IO window plus scheduling "
              "jitter (the cushion whose absence was Defect B)");
static_assert(AudioTimingGeometry::kTxExposureLeadFrames <
                  AudioTimingGeometry::kFrameRingFrames,
              "TX exposure lead cannot exceed the host frame ring");
static_assert(AudioTimingGeometry::kTxExposureLeadPackets <=
                  AudioTimingGeometry::kTxSharedSlotPackets,
              "TX packet lead must be able to hold the required exposure frames");
static_assert(AudioTimingGeometry::kTxFrameExposureWindowPackets *
                  AudioTimingGeometry::kCadenceBlockFrames >=
              (AudioTimingGeometry::kHalIoPeriodFrames +
               AudioTimingGeometry::kTxExposureLeadFrames) *
                  AudioTimingGeometry::kCadenceBlockPackets,
              "TX frame-exposure packet window must cover WriteEnd plus the "
              "exposure cushion");
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
// 32 @192k  (== our kFramesPerDataPacket and AudioGeometryPolicy::FramesPerPacket).
//
// --- Apple AppleFWAudio.kext  (AM824DCLWrite / AM824NuDCLWrite, x86 IDA) ------
//   fNumBufferGroups          = 100        backing DCL ring depth (~100 ms)
//   fNumPacketsPerBufferGroup = 8          => HW interrupt every 8 pkt = 1.0 ms
//   48k cadence               = D,D,D,N    => 6 frames/cycle avg, 8-frame DATA
//   CheckSYT target latency   = 2-3 cycles (~250-375 us) device presentation
//                                          -- this is a SYT/presentation lead
//                                          (cf. our TxTransferDelayTicks/SYT),
//                                          NOT the CoreAudio safety offset.
//   servo update              = gated groupIndex==0 => ~100 ms / ring wrap
//                                          (100 groups * 8 pkt * 125 us)
//   our analogues: 8-pkt group ~ kTimingGroupPackets(6, 0.75 ms); 100*8=800-pkt
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
//   * Everyone services DMA far more often than a deep batch: 0.75-1.375 ms IRQ
//     (6-11 pkt). Ours kTimingGroupPackets = 6 (0.75 ms) is in range.
//   * Everyone keeps SYT/presentation in the 1394 tick domain, not host time.
//   * Backing ring depth (Apple ~800 pkt, ffado 128) is decoupled from the
//     active near-wire lead -- matches our capacity-is-not-latency rule. None of
//     these exposes a direct AudioDriverKit ZTS analogue (see §9.F), so
//     kTxExposureLeadFrames and the ZTS period are OURS to justify, not inherited.
// =============================================================================
