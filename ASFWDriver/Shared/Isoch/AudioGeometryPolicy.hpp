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

namespace ASFW::IsochTransport {

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

    // Reported presentation latency (Saffire kext ladder). Frames.
    static constexpr uint32_t ReportedLatencyFrames(double rate) {
        if (rate > 96000.0) return 119u;
        if (rate > 48000.0) return 59u;
        return 29u;
    }

    // TX exposure cushion -- the symmetric partner of RequiredInputSafetyFrames.
    // The producer must keep E ahead of W by at least one IO window + jitter.
    // Rate-aware view of AudioTimingGeometry::kTxExposureLeadFrames; the
    // producer path is what must ENFORCE the returned value.
    static constexpr uint32_t RequiredOutputExposureFrames(
        uint32_t maxClientIoFrames, uint32_t jitterFrames) {
        return maxClientIoFrames + jitterFrames;
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
    // The TX exposure cushion must be honourable within the ring.
    const uint32_t exposure = AudioGeometryPolicy::RequiredOutputExposureFrames(
        AudioTimingGeometry::kHalIoPeriodFrames,
        AudioTimingGeometry::kSchedulingJitterFrames);
    if (exposure >= AudioTimingGeometry::kFrameRingFrames) {
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

} // namespace ASFW::IsochTransport

// -----------------------------------------------------------------------------
// RX input-safety cushion. Kept in namespace ASFW::Audio (unchanged signature)
// so existing call sites and InputSafetyPolicy.hpp includers do not break.
// -----------------------------------------------------------------------------
namespace ASFW::Audio {

[[nodiscard]] constexpr uint32_t RequiredInputSafetyFrames(
    uint32_t profileInputSafety,
    uint32_t maximumFramesPerInterrupt,
    uint32_t schedulingJitterFrames) noexcept {
    // The HAL safety offset is the data-VISIBILITY margin only: how far the
    // input read head must lag the producer so the frames are guaranteed
    // present. CoreAudio accounts for the IO buffer size SEPARATELY (via the
    // buffer-frame-size property), so the IO window must NOT be folded in here.
    // Folding it in inflated the input safety offset to 624 frames (~13 ms,
    // ~10.7 ms of phantom input latency) and broke the input==output symmetry
    // every reference stack holds: Apogee Duet plist (50/50 @48k, x2/x4 by
    // rate), Focusrite Saffire (delayPackets x framesPerPacket), and Apple's
    // AppleFWAudio engine (small multiples of frames-per-group + device
    // override) all sit at tens of frames, never hundreds.
    //
    // The margin is one interrupt batch + scheduling jitter, floored by the
    // device profile's own value (RxSafetyOffsetFrames = 128 @48k), aligned up
    // to the 32-frame grid.
    const uint32_t interruptBatch =
        maximumFramesPerInterrupt + schedulingJitterFrames;
    const uint32_t raw =
        profileInputSafety > interruptBatch ? profileInputSafety : interruptBatch;
    constexpr uint32_t kAlign =
        ASFW::IsochTransport::AudioTimingGeometry::kFrameAlignment;
    return ((raw + kAlign - 1) / kAlign) * kAlign;
}

} // namespace ASFW::Audio
