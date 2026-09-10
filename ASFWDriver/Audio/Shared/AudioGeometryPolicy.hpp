// =============================================================================
// AudioGeometryPolicy.hpp
//
// SINGLE SOURCE OF TRUTH for rate- and device-DEPENDENT audio geometry: the
// values that are FUNCTIONS of sample rate (frames-per-packet, safety offsets,
// reported latency) and the per-rate validation that ties them back to the
// rate-independent structure in AudioTimingGeometry.hpp.
//
// Why a separate file from AudioTimingGeometry.hpp: these are not constants.
// frames-per-packet is 8/16/32 at 1x/2x/4x; safety offset = delayPackets x
// framesPerPacket. The boundary is semantic (compile-time constant vs
// function-of-rate), not size.
//
// Device profiles (e.g. FocusriteSaffireProfile) should DELEGATE their
// per-rate safety/latency to this header rather than re-deriving the ladder.
// =============================================================================
#pragma once

#include "AudioTimingGeometry.hpp"

#include <cstdint>

namespace ASFW::Audio::Shared {

struct AudioGeometryPolicy final {
    // Rate ladder. frames-per-packet doubles each 2x step; a rate addend pads
    // the safety/latency scaling. (Matches FocusriteSaffireProfile.cpp.)
    static constexpr uint32_t FramesPerPacket(double rate) {
        if (rate > 96000.0) return 32u;
        if (rate > 48000.0) return 16u;
        return 8u;
    }
    static constexpr uint32_t RateAddend(double rate) {
        if (rate > 96000.0) return 4u;
        if (rate > 48000.0) return 2u;
        return 0u;
    }

    // Safety offsets (latencyMode 1). delayPackets x framesPerPacket(rate).
    //   TX (playback): 6 packets  -- smaller, tuned for latency.
    //   RX (capture): 16 packets  -- larger, absorbs reception jitter.
    static constexpr uint32_t kTxDelayPackets = 6;
    static constexpr uint32_t kRxDelayPackets = 16;

    static constexpr uint32_t TxSafetyOffsetFrames(double rate) {     // Frames
        return (kTxDelayPackets + RateAddend(rate)) * FramesPerPacket(rate);
    }
    static constexpr uint32_t RxSafetyOffsetFrames(double rate) {     // Frames
        return (kRxDelayPackets + RateAddend(rate)) * FramesPerPacket(rate);
    }

    // Frames carried by one RX completion batch, counting every packet in the
    // group as DATA -- an upper bound, since the D,D,D,N cadence leaves one
    // NO-DATA per four packets.
    //
    // This is the floor for any capture visibility margin: a safety offset
    // below it does not cover the interval it exists to cover, because a
    // reader can be one whole batch behind the writer between completions. It
    // is deliberately geometry only -- no scheduling-jitter term -- so a
    // profile that says nothing still gets a margin that is defensible rather
    // than arbitrary. See the J3 row in
    // documentation/AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md.
    static constexpr uint32_t CompletionBatchFrames(double rate) {
        return AudioTimingGeometry::kRxPacketsPerGroup * FramesPerPacket(rate);
    }

    // Reported presentation latency (Saffire kext ladder). Frames.
    static constexpr uint32_t ReportedLatencyFrames(double rate) {
        if (rate > 96000.0) return 119u;
        if (rate > 48000.0) return 59u;
        return 29u;
    }

    // -------------------------------------------------------------------------
    // Focusrite Saffire empirical calibration:
    // -------------------------------------------------------------------------
    // Sourced from empirical measurements on Focusrite Saffire Pro 24 DSP:
    //
    // 1. Hardware Converter Latency (ADC + DAC + hardware FIFOs):
    //    - Physical loopback RTL (tools/rtl/rtl_loopback -d "Saffire"):
    //      RTL_ts measured invariant at 105.01 frames @ 48 kHz (sd = 0.00-0.02)
    //      across buffer sizes 512, 128, and 64.
    //    - Oblique Audio RTL Utility validation at 64 samples:
    //      Measured 421 samples vs 374 samples reported = +47 sample uncompensated residual
    //      when driver declared the legacy vendor ladder (29 in / 29 out = 58 frames).
    //    - DAWBench Low Latency Database historical benchmark:
    //      Focusrite Saffire Driver 4.0.0 on Windows 7 x64 explicitly carried footnote:
    //      "* I/O not reporting AD/DA".
    //    - Calibrated symmetrically: 53 in / 52 out = 105 frames total.
    //      Resulting residual drops from +47.01 frames to +0.01 frames (< 0.2 µs),
    //      achieving sub-microsecond, sample-accurate DAW alignment.
    //
    // 2. Safety Offsets:
    //    - RX Safety: Tuned to 10 packets (80 frames @ 48k).
    //      The hardware completion batch floor is 8 packets (64 frames). 10 packets
    //      provides 16 frames (2 packets / 250 µs) of headroom above the DMA floor.
    //      Instruments.app ZTS jitter analysis (SAFFIRE-48k Run 2) verified phase-lock
    //      std dev of 52 ns (max jitter spread 210 ns). 250 µs headroom is >1000x
    //      the physical ZTS jitter. Saves 48 frames (1.0 ms) of RTL over legacy 16 packets.
    //    - TX Safety: 6 packets nominal (clamped to 60 frames / 10 slots by payload-finality).
    //      TX Latency metering (E0 -> E2) verified minimum hardware transmission wait of
    //      2,015.3 µs (96.7 frames) with zero substitutions, leaving a 36.7 frame (765 µs)
    //      cushion above the 60-frame deadline.
    static constexpr uint32_t kSaffireTxDelayPackets = 6;
    static constexpr uint32_t kSaffireRxDelayPackets = 10;

    static constexpr uint32_t SaffireTxSafetyOffsetFrames(double rate) {
        return (kSaffireTxDelayPackets + RateAddend(rate)) * FramesPerPacket(rate);
    }
    static constexpr uint32_t SaffireRxSafetyOffsetFrames(double rate) {
        return (kSaffireRxDelayPackets + RateAddend(rate)) * FramesPerPacket(rate);
    }
    static constexpr uint32_t SaffireReportedInputLatencyFrames(double rate) {
        if (rate > 96000.0) return 212u;
        if (rate > 48000.0) return 106u;
        return 53u;
    }
    static constexpr uint32_t SaffireReportedOutputLatencyFrames(double rate) {
        if (rate > 96000.0) return 208u;
        if (rate > 48000.0) return 104u;
        return 52u;
    }

    // Minimum completed-content publication span: one client operation plus
    // scheduling jitter. It sizes byte retention only and is not a scheduler.
    static constexpr uint32_t RequiredOutputPublicationFrames(
        uint32_t maxClientIoFrames, uint32_t jitterFrames) {
        return maxClientIoFrames + jitterFrames;
    }

    // Conservative hardware-relative output lead. It is derived from the
    // physical payload-finality policy; backend presentation delay is reported
    // separately as latency. Ring/cache capacity is not an input.
    static constexpr uint32_t RequiredOutputSafetyFrames(
        uint32_t profileFloorFrames,
        uint32_t sampleRateHz) noexcept {
        if (!AudioTimingGeometry::IsV3SampleRate(sampleRateHz)) {
            return 0;
        }
        // Safety is the producer-visible payload-finality lead, not the arm
        // horizon and not backend presentation delay. The latter is reported
        // separately as stream latency.
        const uint64_t scheduledFrames =
            (static_cast<uint64_t>(sampleRateHz) *
                 AudioTimingGeometry::kTxContentFreezeCycleSlots +
             7'999U) /
            8'000U;
        return static_cast<uint32_t>(scheduledFrames > profileFloorFrames
            ? scheduledFrames : profileFloorFrames);
    }
};

// =============================================================================
// PER-RATE VALIDATION
// AudioTimingGeometry.hpp asserts the rate-independent structure once. Here we
// assert the rate-DEPENDENT relationships at every supported rate, so a ladder
// edit that breaks 96 or 192 kHz fails the build, not only 48 kHz.
// =============================================================================
namespace detail {

constexpr bool ValidAtRate(double rate) {
    const uint32_t fpp = AudioGeometryPolicy::FramesPerPacket(rate);
    const uint32_t txSafety = AudioGeometryPolicy::TxSafetyOffsetFrames(rate);
    const uint32_t rxSafety = AudioGeometryPolicy::RxSafetyOffsetFrames(rate);

    // framesPerPacket must be a whole multiple of the 1x base.
    if (fpp == 0 || (fpp % AudioTimingGeometry::kFramesPerDataPacket) != 0) {
        return false;
    }
    // RX safety must exceed TX safety (capture absorbs more jitter than playback).
    if (rxSafety <= txSafety) {
        return false;
    }
    // Safety offsets must fit inside the host frame ring.
    if (txSafety >= AudioTimingGeometry::kFrameRingFrames) {
        return false;
    }
    if (rxSafety >= AudioTimingGeometry::kFrameRingFrames) {
        return false;
    }
    // One callback plus jitter must be retainable within the host ring.
    const uint32_t publication =
        AudioGeometryPolicy::RequiredOutputPublicationFrames(
        AudioTimingGeometry::kHalIoPeriodFrames,
        AudioTimingGeometry::kSchedulingJitterFrames);
    if (publication >= AudioTimingGeometry::kFrameRingFrames) {
        return false;
    }
    return true;
}

} // namespace detail

static_assert(detail::ValidAtRate(48000.0),  "geometry policy invalid at 48 kHz");
static_assert(detail::ValidAtRate(96000.0),  "geometry policy invalid at 96 kHz");
static_assert(detail::ValidAtRate(192000.0), "geometry policy invalid at 192 kHz");

// Pin the documented 48 kHz values so a refactor that drifts them fails.
static_assert(AudioGeometryPolicy::TxSafetyOffsetFrames(48000.0) == 48,
              "48k TX safety must be 6 packets x 8 frames");
static_assert(AudioGeometryPolicy::RxSafetyOffsetFrames(48000.0) == 128,
              "48k RX safety must be 16 packets x 8 frames");
static_assert(AudioGeometryPolicy::ReportedLatencyFrames(48000.0) == 29,
              "48k reported latency must be 29 frames");

// Saffire empirical calibration assertions:
static_assert(AudioGeometryPolicy::SaffireTxSafetyOffsetFrames(48000.0) == 48,
              "48k Saffire TX safety must be 6 packets x 8 frames = 48 frames");
static_assert(AudioGeometryPolicy::SaffireRxSafetyOffsetFrames(48000.0) == 80,
              "48k Saffire RX safety must be 10 packets x 8 frames = 80 frames");
static_assert(AudioGeometryPolicy::SaffireReportedInputLatencyFrames(48000.0) +
              AudioGeometryPolicy::SaffireReportedOutputLatencyFrames(48000.0) == 105,
              "48k Saffire reported roundtrip latency must be 105 frames (53 in / 52 out)");

// With payload finality decoupled from the 8-packet completion group and
// pegged to the physical prefetch limit (kPayloadRepointGuardPackets = 2)
// plus 1 slack packet = 3 packets (18 frames nominal @ 48k), the physical
// freeze frontier no longer inflates the profile floor. The device profile's
// floor (e.g. Duet's 50 frames, Saffire's 48 frames) governs the safety offset.
static_assert(AudioGeometryPolicy::RequiredOutputSafetyFrames(
                  50, 48'000) == 50,
              "48k safety reflects device profile floor when finality frontier is lower");

} // namespace ASFW::Audio::Shared
