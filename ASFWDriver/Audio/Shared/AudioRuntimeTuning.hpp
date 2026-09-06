// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Runtime-tunable audio geometry.
//
// Every value here has a compile-time counterpart in AudioTimingGeometry /
// AudioHalBufferProfiles that is still the default. This type exists so the
// values can be swept from the app without a rebuild-and-reinstall cycle, which
// is what makes a wrong hypothesis expensive. It is a *diagnostic* surface: the
// defaults remain the shipping configuration and Validate() says plainly when a
// candidate leaves the range the compile-time asserts guarantee.
//
// Three groups, and they do NOT cost the same to apply:
//
//   TransmitDepth  -> stream re-arm. RequestDeviceConfigurationChange stops IO,
//                     we re-read the geometry, the HAL restarts IO.
//   Declarations   -> stream re-arm. Re-issued through SetOutputLatency and
//                     friends inside the same window.
//   HalGeometry    -> device re-publish. The zero-timestamp period is fixed at
//                     IOUserAudioDevice::init() (ASFWAudioDriverGraph.cpp:253),
//                     and the frame ring sizes cross-process shared memory, so
//                     neither can change under a live device.
//
// Nothing here is a claim that a value is safe. ValidationOutcome separates
// "cannot be applied" from "outside the asserted envelope, applied because the
// operator asked" -- the second is the whole point of the panel, and the caller
// is required to look at it rather than test a bool.

#pragma once

#include "AudioHalBufferProfiles.hpp"
#include "AudioTimingGeometry.hpp"

#include <cstdint>

namespace ASFW::Audio::Shared {

// Which groups a request wants applied. A request that touches no group is a
// query, not a change, and must not restart anything.
enum class TuningGroup : uint32_t {
    kNone = 0,
    kTransmitDepth = 1u << 0,
    kDeclarations = 1u << 1,
    kHalGeometry = 1u << 2,
};

[[nodiscard]] constexpr uint32_t operator|(TuningGroup a, TuningGroup b) noexcept {
    return static_cast<uint32_t>(a) | static_cast<uint32_t>(b);
}
// Mixed overloads so a three-way `a | b | c` folds without the caller casting;
// the accumulator is the mask type the wire format carries.
[[nodiscard]] constexpr uint32_t operator|(uint32_t mask, TuningGroup g) noexcept {
    return mask | static_cast<uint32_t>(g);
}
[[nodiscard]] constexpr uint32_t operator|(TuningGroup g, uint32_t mask) noexcept {
    return static_cast<uint32_t>(g) | mask;
}
[[nodiscard]] constexpr bool Contains(uint32_t mask, TuningGroup g) noexcept {
    return (mask & static_cast<uint32_t>(g)) != 0;
}

// What applying a request costs the user. Reported back so the UI can say so
// before the operator presses Apply, rather than surprising them with a device
// that vanishes from every running app.
enum class ApplyCost : uint32_t {
    kNothing = 0,     // no group selected, or every value already current
    kStreamRearm = 1, // IO stops and restarts; the device stays published
    kDeviceRepublish = 2, // the CoreAudio device disappears and reappears
};

enum class TuningRejection : uint32_t {
    kNone = 0,
    kPreparedTargetExceedsSharedSlots,
    kDispatchSlackZero,
    kOwnershipGuardMismatch,
    kFrameRingNotMultipleOfIoBudget,
    kFrameRingNotMultipleOfZtsPeriod,
    kZtsPeriodNotMultipleOfMaxPacketFrames,
    kZtsPeriodZero,
    kIoBudgetZero,
    kFrameRingZero,
    kSafetyOffsetExceedsFrameRing,
    kLatencyExceedsFrameRing,
    kUnsupportedGroups,
    kBusy,
    kNotReady,
    kRequestIdExhausted,
};

// Non-fatal: the candidate is applicable but leaves the envelope the
// compile-time asserts guarantee. Reported, never silently swallowed.
enum class TuningWarning : uint32_t {
    kNone = 0,
    // AudioTimingGeometry.hpp:214 -- "Do not take it below 12 [groups]:
    // overrunning it holes the descriptor ring, and unlike a PCM gap that is a
    // transport failure silence substitution cannot cover." The shipping value
    // sits exactly on this bound, so every reduction crosses it.
    kDispatchSlackBelowAssertedFloor = 1u << 0,
    // The shared slot ring keeps its compile-time size, so a reduced prepared
    // target simply leaves it over-provisioned. Harmless, but worth saying.
    kSharedSlotRingOverProvisioned = 1u << 1,
    // Declaring less than we measure is what produces a misaligned recording.
    kDeclaredLatencyBelowDefault = 1u << 2,
};

struct AudioRuntimeTuning final {
    // --- TransmitDepth -----------------------------------------------------
    // Preparation runs to `completion + (ownershipGuard + dispatchSlack)`.
    uint32_t txDispatchSlackPackets{
        AudioTimingGeometry::kTxDispatchSlackCycleSlots};
    uint32_t txOwnershipGuardPackets{
        AudioTimingGeometry::kTxOwnershipGuardCycleSlots};

    // --- Declarations ------------------------------------------------------
    // What CoreAudio is told. Zero means "keep whatever the device profile
    // resolved", which is the only way to express "do not override".
    uint32_t outputLatencyFrames{0};
    uint32_t inputLatencyFrames{0};
    uint32_t outputSafetyOffsetFrames{0};
    uint32_t inputSafetyOffsetFrames{0};

    // --- HalGeometry -------------------------------------------------------
    uint32_t frameRingFrames{kActiveAudioHalBufferProfile.frameRingFrames};
    uint32_t clientIoBudgetFrames{
        kActiveAudioHalBufferProfile.clientIoBudgetFrames};
    uint32_t zeroTimestampPeriodFrames{
        kActiveAudioHalBufferProfile.zeroTimestampPeriodFrames};

    [[nodiscard]] constexpr uint32_t PreparedTargetPackets() const noexcept {
        return txOwnershipGuardPackets + txDispatchSlackPackets;
    }

    // The producer must absorb one coalesced completion delta without holing
    // the descriptor ring. This is the quantity AudioTimingGeometry.hpp:214
    // bounds at twelve completion groups.
    [[nodiscard]] constexpr uint32_t MaxCoveredDeltaConsumedPackets()
        const noexcept {
        return txDispatchSlackPackets;
    }

    [[nodiscard]] constexpr bool operator==(
        const AudioRuntimeTuning&) const noexcept = default;
};

struct ValidationOutcome final {
    TuningRejection rejection{TuningRejection::kNone};
    uint32_t warnings{0};

    [[nodiscard]] constexpr bool Applicable() const noexcept {
        return rejection == TuningRejection::kNone;
    }
    [[nodiscard]] constexpr bool Has(TuningWarning w) const noexcept {
        return (warnings & static_cast<uint32_t>(w)) != 0;
    }
};

// `defaults` is what the shipping constants say, so a candidate can be judged
// against the configuration the asserts were written for rather than against
// itself.
[[nodiscard]] constexpr ValidationOutcome ValidateTuning(
    const AudioRuntimeTuning& candidate,
    const AudioRuntimeTuning& defaults = {}) noexcept {
    ValidationOutcome out{};

    // --- TransmitDepth -----------------------------------------------------
    if (candidate.txDispatchSlackPackets == 0) {
        if (out.rejection == TuningRejection::kNone)
            out.rejection = TuningRejection::kDispatchSlackZero;
    }
    // The guard is the hardware ring and is not tunable: the descriptor ring's
    // size is fixed at build time, so a request that disagrees is malformed
    // rather than merely aggressive.
    if (candidate.txOwnershipGuardPackets !=
        AudioTimingGeometry::kTxOwnershipGuardCycleSlots) {
        if (out.rejection == TuningRejection::kNone)
            out.rejection = TuningRejection::kOwnershipGuardMismatch;
    }
    // Shared slot storage keeps its compile-time size because it is
    // cross-process memory. AudioTimingGeometry.hpp:222 fixes that size as
    // `prepared target + ownership guard`, so the store must still hold both
    // after the target shrinks -- the target alone is not the bound.
    const uint64_t required = uint64_t{candidate.txDispatchSlackPackets} +
                              2ULL * candidate.txOwnershipGuardPackets;
    if (required > AudioTimingGeometry::kTxSharedSlotPackets) {
        if (out.rejection == TuningRejection::kNone)
            out.rejection = TuningRejection::kPreparedTargetExceedsSharedSlots;
    } else if (required < AudioTimingGeometry::kTxSharedSlotPackets) {
        out.warnings |= static_cast<uint32_t>(TuningWarning::kSharedSlotRingOverProvisioned);
    }
    if (candidate.MaxCoveredDeltaConsumedPackets() <
        12U * AudioTimingGeometry::kTxPacketsPerGroup) {
        out.warnings |= static_cast<uint32_t>(TuningWarning::kDispatchSlackBelowAssertedFloor);
    }

    // --- HalGeometry -------------------------------------------------------
    // Mirrors IsValidAudioHalBufferProfile, split so the UI can name the field
    // that failed instead of reporting one opaque bool.
    if (candidate.frameRingFrames == 0) {
        if (out.rejection == TuningRejection::kNone)
            out.rejection = TuningRejection::kFrameRingZero;
    }
    if (candidate.clientIoBudgetFrames == 0) {
        if (out.rejection == TuningRejection::kNone)
            out.rejection = TuningRejection::kIoBudgetZero;
    }
    if (candidate.zeroTimestampPeriodFrames == 0) {
        if (out.rejection == TuningRejection::kNone)
            out.rejection = TuningRejection::kZtsPeriodZero;
    }
    if (candidate.clientIoBudgetFrames != 0 &&
        candidate.frameRingFrames % candidate.clientIoBudgetFrames != 0) {
        if (out.rejection == TuningRejection::kNone)
            out.rejection = TuningRejection::kFrameRingNotMultipleOfIoBudget;
    }
    if (candidate.zeroTimestampPeriodFrames != 0 &&
        candidate.frameRingFrames % candidate.zeroTimestampPeriodFrames != 0) {
        if (out.rejection == TuningRejection::kNone)
            out.rejection = TuningRejection::kFrameRingNotMultipleOfZtsPeriod;
    }
    if (candidate.zeroTimestampPeriodFrames % kMaxBlockingFramesPerDataPacket !=
        0) {
        if (out.rejection == TuningRejection::kNone)
            out.rejection = TuningRejection::kZtsPeriodNotMultipleOfMaxPacketFrames;
    }

    // --- Declarations ------------------------------------------------------
    // AudioGeometryPolicy.hpp:125-136 rejects a safety offset that reaches the
    // frame ring; the same bound is the only hard limit on a declaration.
    if (candidate.outputSafetyOffsetFrames >= candidate.frameRingFrames ||
        candidate.inputSafetyOffsetFrames >= candidate.frameRingFrames) {
        if (out.rejection == TuningRejection::kNone)
            out.rejection = TuningRejection::kSafetyOffsetExceedsFrameRing;
    }
    if (candidate.outputLatencyFrames >= candidate.frameRingFrames ||
        candidate.inputLatencyFrames >= candidate.frameRingFrames) {
        if (out.rejection == TuningRejection::kNone)
            out.rejection = TuningRejection::kLatencyExceedsFrameRing;
    }
    if ((candidate.outputLatencyFrames != 0 &&
         candidate.outputLatencyFrames < defaults.outputLatencyFrames) ||
        (candidate.inputLatencyFrames != 0 &&
         candidate.inputLatencyFrames < defaults.inputLatencyFrames)) {
        out.warnings |= static_cast<uint32_t>(TuningWarning::kDeclaredLatencyBelowDefault);
    }
    return out;
}

[[nodiscard]] constexpr ApplyCost CostOf(uint32_t groups,
                                         const AudioRuntimeTuning& candidate,
                                         const AudioRuntimeTuning& current) noexcept {
    if (Contains(groups, TuningGroup::kHalGeometry) &&
        (candidate.frameRingFrames != current.frameRingFrames ||
         candidate.clientIoBudgetFrames != current.clientIoBudgetFrames ||
         candidate.zeroTimestampPeriodFrames !=
             current.zeroTimestampPeriodFrames)) {
        return ApplyCost::kDeviceRepublish;
    }
    const bool depth =
        Contains(groups, TuningGroup::kTransmitDepth) &&
        (candidate.txDispatchSlackPackets != current.txDispatchSlackPackets ||
         candidate.txOwnershipGuardPackets != current.txOwnershipGuardPackets);
    const bool declarations =
        Contains(groups, TuningGroup::kDeclarations) &&
        (candidate.outputLatencyFrames != current.outputLatencyFrames ||
         candidate.inputLatencyFrames != current.inputLatencyFrames ||
         candidate.outputSafetyOffsetFrames != current.outputSafetyOffsetFrames ||
         candidate.inputSafetyOffsetFrames != current.inputSafetyOffsetFrames);
    return (depth || declarations) ? ApplyCost::kStreamRearm
                                   : ApplyCost::kNothing;
}

// ---------------------------------------------------------------------------
// The arithmetic CoreAudio and its clients perform over our declarations.
//
// This is NOT a latency measurement and must never be presented as one. It is
// exactly the sum a host computes from the four numbers we declare plus its own
// buffer size -- the same sum Logic prints as "Resulting Latency" -- so the
// panel can show what the host will believe next to what rtl_loopback measures.
// The gap between them is the residual, and it is the interesting quantity.
struct DeclaredLatencyMath final {
    uint32_t clientIoBufferFrames{0};
    uint32_t outputPathFrames{0};   // io + out safety + out latency
    uint32_t inputPathFrames{0};    // io + in safety + in latency
    uint32_t roundTripFrames{0};
    uint32_t schedulingDistanceFrames{0}; // 2*io + both safety offsets
    uint32_t declaredHardwareFrames{0};   // both declared latencies

    [[nodiscard]] constexpr uint32_t MillisecondsQ16(
        uint32_t frames, uint32_t sampleRateHz) const noexcept {
        if (sampleRateHz == 0) return 0;
        return static_cast<uint32_t>((static_cast<uint64_t>(frames) * 1000ULL
                                      << 16) /
                                     sampleRateHz);
    }
};

[[nodiscard]] constexpr DeclaredLatencyMath ComputeDeclaredLatency(
    const AudioRuntimeTuning& tuning, uint32_t clientIoBufferFrames) noexcept {
    DeclaredLatencyMath m{};
    m.clientIoBufferFrames = clientIoBufferFrames;
    m.outputPathFrames = clientIoBufferFrames + tuning.outputSafetyOffsetFrames +
                         tuning.outputLatencyFrames;
    m.inputPathFrames = clientIoBufferFrames + tuning.inputSafetyOffsetFrames +
                        tuning.inputLatencyFrames;
    m.roundTripFrames = m.outputPathFrames + m.inputPathFrames;
    m.schedulingDistanceFrames = 2U * clientIoBufferFrames +
                                 tuning.outputSafetyOffsetFrames +
                                 tuning.inputSafetyOffsetFrames;
    m.declaredHardwareFrames =
        tuning.outputLatencyFrames + tuning.inputLatencyFrames;
    return m;
}

// Nominal planning horizon, not measured latency. Bus cycles keep their
// duration as sample rate changes; the number of audio frames must scale.
[[nodiscard]] constexpr uint32_t PreparedLeadFrames(
    const AudioRuntimeTuning& tuning, uint32_t sampleRateHz) noexcept {
    return static_cast<uint32_t>(uint64_t{tuning.PreparedTargetPackets()} *
                                 sampleRateHz / 8'000U);
}

// Isochronous cycles are 125 us regardless of sample rate, so a packet count
// converts to time without knowing the rate.
[[nodiscard]] constexpr uint32_t PacketsToMicroseconds(
    uint32_t packets) noexcept {
    return packets * 125U;
}

static_assert(ValidateTuning(AudioRuntimeTuning{}).Applicable(),
              "the shipping defaults must validate");
static_assert(AudioRuntimeTuning{}.PreparedTargetPackets() ==
                  AudioTimingGeometry::kTxPreparedTargetCycleSlots,
              "default tuning must reproduce the compile-time prepared target");
static_assert(PreparedLeadFrames(AudioRuntimeTuning{}, 48'000) == 720,
              "default prepared lead is 120 packets x 6 frames");
// The shipping value sits exactly on the asserted floor, so it must not warn,
// and anything below it must.
static_assert(!ValidateTuning(AudioRuntimeTuning{})
                   .Has(TuningWarning::kDispatchSlackBelowAssertedFloor));
// The defaults exactly fill the shared slot store, so they must not report it
// as over-provisioned either -- that warning is reserved for real reductions.
static_assert(!ValidateTuning(AudioRuntimeTuning{})
                   .Has(TuningWarning::kSharedSlotRingOverProvisioned));

} // namespace ASFW::Audio::Shared
