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

    // Backing packet-ring / timeline slot array length
    // (AmdtpPacketTimeline, DiceTxStreamEngine::timelineSlots_). Packets.
    static constexpr uint32_t kTimelineSlots = 512;

    // === TX exposure lead (E - W) -- the audio-frame cushion ================
    // Minimum audio frames the producer's exposure frontier (E) must keep ahead
    // of CoreAudio's write-window leading edge (W). TX analogue of RX's
    // RequiredInputSafetyFrames cushion: RX had it, TX did not -- which is why
    // TX shipped ~1% silence (Defect B = under-exposure, W > E). Previously
    // IMPLICIT (emerged from AlignFrameCursorOnce + the per-packet advance,
    // measured ~120 frames against a 128-frame IO window). Named so it can be
    // asserted and, in the follow-up fix, enforced by the producer. Conservative
    // form = one full client-IO budget + scheduling jitter; tighten toward the
    // actual IO size once measured. Frames.
    static constexpr uint32_t kTxExposureLeadFrames =
        kHalIoPeriodFrames + kSchedulingJitterFrames;          // 512 + 64 = 576
    // Packet lead deep enough to expose that many frames (ceil at 6 fr/pkt avg,
    // computed x100 to stay integral). ceil(576 / 6) = 96 packets.
    static constexpr uint32_t kTxExposureLeadPackets =
        (kTxExposureLeadFrames * 100 +
         (kCadenceBlockFrames * 100 / kCadenceBlockPackets) - 1) /
        (kCadenceBlockFrames * 100 / kCadenceBlockPackets);
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
static_assert(AudioTimingGeometry::kTxSharedSlotPackets <=
                  AudioTimingGeometry::kTimelineSlots,
              "shared packet ring must fit inside the timeline slot array");

} // namespace ASFW::IsochTransport
