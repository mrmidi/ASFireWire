// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuV3TxTimingStamper.cpp - see MotuV3TxTimingStamper.hpp.

#include "MotuV3TxTimingStamper.hpp"

#include "../AMDTP/MotuV3WireFormat.hpp"

namespace ASFW::Audio::Wire {
namespace {

namespace AMDTP = ::ASFW::Protocols::Audio::AMDTP;
namespace MOTU = ::ASFW::Audio::MOTU;

// The servo's control horizon: one decision, and one spread phase correction,
// per 512 frames. This is the horizon every lock margin was measured on, when it
// was the HAL client IO budget of every buffer profile. That budget has since
// been raised to 1024 frames (kHalIoPeriodFrames); following it would halve the
// loop's gain per frame and stretch the acquisition mute from two 512-frame
// updates (22 ms) to 43 ms. So the servo states its own horizon instead.
constexpr uint32_t kServoControlHorizonFrames = 512;

constexpr uint32_t kMotuOneXMinimumTrackingRateHz = 41895;
constexpr uint32_t kMotuOneXMaximumTrackingRateHz = 50160;

constexpr uint32_t kServoFlagConfigured = 1u << 0;
constexpr uint32_t kServoFlagFeedbackUpdated = 1u << 1;
constexpr uint32_t kServoFlagPhaseReferenceReset = 1u << 2;
constexpr uint32_t kServoFlagStepClamped = 1u << 3;
constexpr uint32_t kServoFlagHardResyncRequired = 1u << 4;
constexpr uint32_t kServoFlagPhaseRepairApplied = 1u << 5;
constexpr uint32_t kServoFlagOutputMuted = 1u << 6;
constexpr uint32_t kServoFlagLocked = 1u << 7;
constexpr uint32_t kServoFlagBridgeStalled = 1u << 8;

[[nodiscard]] constexpr int64_t StepQ32ForRate(uint32_t sampleRateHz) noexcept {
    return sampleRateHz == 0
               ? 0
               : static_cast<int64_t>((static_cast<__int128>(AMDTP::MotuV3Wire::kTicksPerSecond)
                                       << MOTU::MotuSphClockServo::kFractionBits) /
                                      sampleRateHz);
}

[[nodiscard]] bool BuildServoConfig(const ::ASFW::Isoch::Audio::AudioStreamConfig& txConfig,
                                    MOTU::MotuSphServoConfig& outConfig) noexcept {
    if (txConfig.sampleRate != 48000 || txConfig.framesPerDataPacket != 8) {
        return false;
    }

    // Behavioral contract distilled from the original FireWire path: phase
    // correction is spread across one client buffer, while its inter-sample
    // limiter spans the observed one-rate-family tracking range. The 8-second
    // SPH wrap domain is unrelated to this 512-frame control horizon.
    //
    // Both gains are stated here rather than left to the controller's defaults,
    // because the two-stage loop is a policy chosen on measured margins,
    // not a property of the controller: 1/D while acquiring so the muted window
    // stays at one update, 1/(4*D) once locked, where the original sits.
    outConfig = {
        .sampleRateHz = txConfig.sampleRate,
        .phaseCorrectionHorizonFrames = kServoControlHorizonFrames,
        .phaseGainShift = 2,
        .acquisitionPhaseGainShift = 0,
        .minimumStepQ32 = StepQ32ForRate(kMotuOneXMaximumTrackingRateHz),
        .maximumStepQ32 = StepQ32ForRate(kMotuOneXMinimumTrackingRateHz),
        .phaseInjectionTicks = kMotuSphPhaseInjectionTicks,
        .phaseInjectionAfterFrames = kMotuSphPhaseInjectionAfterFrames,
    };
    return true;
}

inline void WriteBE32(uint8_t* bytes, uint32_t value) noexcept {
    bytes[0] = static_cast<uint8_t>(value >> 24);
    bytes[1] = static_cast<uint8_t>(value >> 16);
    bytes[2] = static_cast<uint8_t>(value >> 8);
    bytes[3] = static_cast<uint8_t>(value);
}

} // namespace

bool MotuV3TxTimingStamper::Configure(
    const ::ASFW::Isoch::Audio::AudioStreamConfig& txConfig) noexcept {
    MOTU::MotuSphServoConfig servoConfig{};
    if (!BuildServoConfig(txConfig, servoConfig) || !clock_.Configure(txConfig.sampleRate) ||
        !servo_.Configure(servoConfig)) {
        return false;
    }
    updateIntervalFrames_ = servoConfig.phaseCorrectionHorizonFrames;
    ResetForStart();
    return true;
}

void MotuV3TxTimingStamper::ResetForStart() noexcept {
    clock_.Reset();
    servo_.Reset();
    lastFirstSph_ = 0;
    // The output starts muted and stays muted until the loop
    // reports a locked decision, which takes at most two update intervals
    // (22 ms). Armed only where a servo will actually produce those decisions:
    // a latch nobody clears would be permanent silence rather than a short
    // acquisition window.
    const bool acquisitionMute = servo_.IsConfigured();
    gate_.Reset(acquisitionMute);
    streamGeneration_ = 0;
    lastBridgeUpdates_ = 0;
    lastSeenRxFrames_ = 0;
    lastSeenRxTicks_ = 0;
    lastAppliedRxFrames_ = 0;
    haveRelPhase_ = false;
    telemetryDecisions_.store(0, std::memory_order_relaxed);
    // Per start, like `decisions`: the question these answer -- did this run's
    // device ever stop or reverse a counter -- is about one stream, and a total
    // carried across starts would not be readable against one run's records.
    telemetryObservationDrops_.store(0, std::memory_order_relaxed);
    telemetryDropBridgeNotAdvanced_.store(0, std::memory_order_relaxed);
    telemetryDropBridgeRegressed_.store(0, std::memory_order_relaxed);
    telemetryDropRxFramesNotAdvanced_.store(0, std::memory_order_relaxed);
    telemetryDropRxFramesRegressed_.store(0, std::memory_order_relaxed);
    telemetryDropRxTicksNotAdvanced_.store(0, std::memory_order_relaxed);
    telemetryDropRxTicksRegressed_.store(0, std::memory_order_relaxed);
    telemetryHardResyncRequests_.store(0, std::memory_order_relaxed);
    telemetryPhaseRepairs_.store(0, std::memory_order_relaxed);
    // The acquisition mute is a mute the run really had, so it counts: a clean
    // run reads 1/1 here, and a second pair means the valve fired.
    telemetryMuteTransitions_.store(acquisitionMute ? 1 : 0, std::memory_order_relaxed);
    telemetryUnmuteTransitions_.store(0, std::memory_order_relaxed);
    telemetryBridgeUpdates_.store(0, std::memory_order_relaxed);
    telemetryStreamGeneration_.store(0, std::memory_order_relaxed);
    telemetryRxFrames_.store(0, std::memory_order_relaxed);
    telemetryRxTicks_.store(0, std::memory_order_relaxed);
    telemetryTxCorrectionQ32_.store(0, std::memory_order_relaxed);
    telemetryStepQ32_.store(clock_.StepQ32(), std::memory_order_relaxed);
    telemetryMeasuredStepQ32_.store(0, std::memory_order_relaxed);
    telemetryPhaseErrorTicks_.store(0, std::memory_order_relaxed);
    // Report the acquisition mute from the first instant it is real, not from
    // the first decision: between the two the output is already silent, and a
    // snapshot that said otherwise would be the one read by whoever is asking
    // why the stream starts quiet.
    telemetryFlags_.store((servo_.IsConfigured() ? kServoFlagConfigured : 0u) |
                              (acquisitionMute ? kServoFlagOutputMuted : 0u),
                          std::memory_order_release);
}

::ASFW::Audio::TxTimingStampResult MotuV3TxTimingStamper::StampPacket(
    const AMDTP::TxPacketSlotView& slot,
    const AMDTP::PreparedTxPacket& packet,
    const AMDTP::AmdtpTimingState& timing) noexcept {
    if (slot.bytes == nullptr || packet.dbs == 0 || packet.framesInPacket == 0) {
        return ::ASFW::Audio::TxTimingStampResult::kNotApplicable;
    }

    // MOTU V3 carries presentation time in each data block's SPH and leaves
    // the CIP SYT at NO_INFO. Seed once per start from the first timed DATA
    // packet's transmit time, then free-run one sample period per block. A
    // packet with no transmit time before that seed still goes out as DATA,
    // with SPH zero -- see the header for why it is never reverted to NO-DATA.
    if (timing.transmitCycleValid) {
        (void)clock_.SeedOnce(timing.transmitTicks + AMDTP::MotuV3Wire::kPresentationLeadTicks);
    }

    const uint32_t blockBytes = packet.dbs * AMDTP::MotuV3Wire::kBytesPerQuadlet;
    for (uint32_t frame = 0; frame < packet.framesInPacket; ++frame) {
        const uint32_t offset = AMDTP::MotuV3Wire::kCipHeaderBytes + frame * blockBytes;
        if (offset + AMDTP::MotuV3Wire::kSphBytes > packet.byteCount) {
            break;
        }
        const uint32_t sph = clock_.CurrentSph();
        if (frame == 0) {
            lastFirstSph_ = sph;
        }
        WriteBE32(slot.bytes + offset, sph);
        clock_.Advance();
    }
    return ::ASFW::Audio::TxTimingStampResult::kOk;
}

uint8_t MotuV3TxTimingStamper::ClassifyAndCountDrop(
    const MotuSphServoRuntimeInput& input) noexcept {
    uint8_t causes = 0;
    if (input.bridgeUpdates == lastBridgeUpdates_) {
        causes |= kMotuSphDropBridgeNotAdvanced;
    } else if (input.bridgeUpdates < lastBridgeUpdates_) {
        causes |= kMotuSphDropBridgeRegressed;
    }
    if (input.rxFrames == lastSeenRxFrames_) {
        causes |= kMotuSphDropRxFramesNotAdvanced;
    } else if (input.rxFrames < lastSeenRxFrames_) {
        causes |= kMotuSphDropRxFramesRegressed;
    }
    if (input.rxTicks == lastSeenRxTicks_) {
        causes |= kMotuSphDropRxTicksNotAdvanced;
    } else if (input.rxTicks < lastSeenRxTicks_) {
        causes |= kMotuSphDropRxTicksRegressed;
    }
    if (causes == 0) {
        return 0;
    }

    telemetryObservationDrops_.fetch_add(1, std::memory_order_relaxed);
    if ((causes & kMotuSphDropBridgeNotAdvanced) != 0) {
        telemetryDropBridgeNotAdvanced_.fetch_add(1, std::memory_order_relaxed);
    }
    if ((causes & kMotuSphDropBridgeRegressed) != 0) {
        telemetryDropBridgeRegressed_.fetch_add(1, std::memory_order_relaxed);
    }
    if ((causes & kMotuSphDropRxFramesNotAdvanced) != 0) {
        telemetryDropRxFramesNotAdvanced_.fetch_add(1, std::memory_order_relaxed);
    }
    if ((causes & kMotuSphDropRxFramesRegressed) != 0) {
        telemetryDropRxFramesRegressed_.fetch_add(1, std::memory_order_relaxed);
    }
    if ((causes & kMotuSphDropRxTicksNotAdvanced) != 0) {
        telemetryDropRxTicksNotAdvanced_.fetch_add(1, std::memory_order_relaxed);
    }
    if ((causes & kMotuSphDropRxTicksRegressed) != 0) {
        telemetryDropRxTicksRegressed_.fetch_add(1, std::memory_order_relaxed);
    }
    return causes;
}

MotuSphServoApplyResult MotuV3TxTimingStamper::ApplyServoInput(
    const MotuSphServoRuntimeInput& input) noexcept {
    MotuSphServoApplyResult result{};
    result.decision = servo_.Current();

    if (!servo_.IsConfigured() || !input.valid || input.streamGeneration == 0 ||
        input.streamGeneration != input.expectedStreamGeneration || input.bridgeUpdates == 0 ||
        input.rxFrames == 0 || input.rxTicks <= 0) {
        return result;
    }

    const bool newGeneration = input.streamGeneration != streamGeneration_;
    if (!newGeneration) {
        // Drop unless all three counters advanced, with the cause recorded.
        // A silent drop here would make the servo's own regression counters
        // unreachable in production, and their zeros a guarantee of this gate
        // rather than a measurement of the device.
        result.dropCauses = ClassifyAndCountDrop(input);
        if (result.dropCauses != 0) {
            return result;
        }
    }

    if (newGeneration) {
        servo_.Reset();
        // A new generation is a new acquisition, muted exactly like a start.
        // Counted only when it actually silences an audible stream: the first
        // observation of every stream lands here right after ResetForStart
        // already armed the mute, and counting that twice would make the
        // acquisition window look like two.
        const bool wasAudible = !gate_.IsMuted();
        gate_.Reset(true);
        if (wasAudible) {
            telemetryMuteTransitions_.fetch_add(1, std::memory_order_relaxed);
        }
        streamGeneration_ = input.streamGeneration;
        lastBridgeUpdates_ = 0;
        lastSeenRxFrames_ = 0;
        lastSeenRxTicks_ = 0;
        lastAppliedRxFrames_ = 0;
        haveRelPhase_ = false;
    }

    lastBridgeUpdates_ = input.bridgeUpdates;
    lastSeenRxFrames_ = input.rxFrames;
    lastSeenRxTicks_ = input.rxTicks;

    if (lastAppliedRxFrames_ != 0 &&
        input.rxFrames - lastAppliedRxFrames_ < updateIntervalFrames_) {
        return result;
    }

    // The reference of this generation was established about 30 ms in, while
    // the conditioner needs ~12 s of telemetry windows before it can offer an
    // operating point. So the arrival of the first absolute phase has to
    // re-reference the loop; without it the seed stays zero for the whole
    // stream, which is what an earlier hardware run measured.
    const bool relPhaseAcquired = input.haveRelPhase && !haveRelPhase_;
    if (input.haveRelPhase) {
        haveRelPhase_ = true;
    }

    result.decision = servo_.Update({
        .valid = true,
        .generation = input.streamGeneration,
        .rxFrames = input.rxFrames,
        .rxTicks = input.rxTicks,
        .txCorrectionQ32 = clock_.AppliedCorrectionQ32(),
        .haveAbsolutePhaseError = input.haveRelPhase,
        .absolutePhaseErrorTicks = input.relPhaseTicks,
        .haveAbsolutePhaseCenter = input.haveRelPhase,
        .absolutePhaseCenterTicks = input.relPhaseCenterTicks,
        .absolutePhaseAcquired = relPhaseAcquired,
    });
    result.observationApplied = true;
    const auto hardAction = gate_.Update(result.decision.hardResyncRequired, result.decision.locked);
    result.muteActivated = hardAction.muteBeforeRepair;
    result.unmuteActivated = hardAction.unmute;
    result.outputMuted = hardAction.muted;
    if (hardAction.phaseRepairRequired) {
        telemetryHardResyncRequests_.fetch_add(1, std::memory_order_relaxed);
        result.phaseRepairApplied = clock_.ApplyPhaseCorrectionQ32(result.decision.phaseErrorQ32);
        if (result.phaseRepairApplied) {
            telemetryPhaseRepairs_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (result.muteActivated) {
        telemetryMuteTransitions_.fetch_add(1, std::memory_order_relaxed);
    }
    if (result.unmuteActivated) {
        telemetryUnmuteTransitions_.fetch_add(1, std::memory_order_relaxed);
    }
    result.stepApplied = result.decision.valid && clock_.SetStepQ32(result.decision.stepQ32);
    lastAppliedRxFrames_ = input.rxFrames;

    uint32_t flags = kServoFlagConfigured;
    if (result.decision.feedbackUpdated) {
        flags |= kServoFlagFeedbackUpdated;
    }
    if (result.decision.phaseReferenceReset) {
        flags |= kServoFlagPhaseReferenceReset;
    }
    if (result.decision.stepClamped) {
        flags |= kServoFlagStepClamped;
    }
    if (result.decision.hardResyncRequired) {
        flags |= kServoFlagHardResyncRequired;
    }
    if (result.phaseRepairApplied) {
        flags |= kServoFlagPhaseRepairApplied;
    }
    if (result.outputMuted) {
        flags |= kServoFlagOutputMuted;
    }
    if (result.decision.locked) {
        flags |= kServoFlagLocked;
    }
    if (result.decision.bridgeStalled) {
        flags |= kServoFlagBridgeStalled;
    }

    telemetryBridgeUpdates_.store(input.bridgeUpdates, std::memory_order_relaxed);
    telemetryStreamGeneration_.store(input.streamGeneration, std::memory_order_relaxed);
    telemetryRxFrames_.store(input.rxFrames, std::memory_order_relaxed);
    telemetryRxTicks_.store(input.rxTicks, std::memory_order_relaxed);
    telemetryTxCorrectionQ32_.store(clock_.AppliedCorrectionQ32(), std::memory_order_relaxed);
    telemetryStepQ32_.store(result.decision.stepQ32, std::memory_order_relaxed);
    telemetryMeasuredStepQ32_.store(result.decision.measuredStepQ32, std::memory_order_relaxed);
    telemetryPhaseErrorTicks_.store(result.decision.phaseErrorTicks, std::memory_order_relaxed);
    telemetryReferenceCause_.store(static_cast<uint32_t>(result.decision.referenceCause),
                                   std::memory_order_relaxed);
    telemetrySeedReReferences_.store(result.decision.seedReReferenceCount,
                                     std::memory_order_relaxed);
    telemetryRxFrameRegressions_.store(result.decision.rxFrameRegressionCount,
                                       std::memory_order_relaxed);
    telemetryRxTickRegressions_.store(result.decision.rxTickRegressionCount,
                                      std::memory_order_relaxed);
    telemetryNonAdvancingTicks_.store(result.decision.nonAdvancingTickCount,
                                      std::memory_order_relaxed);
    telemetryBridgeStalls_.store(result.decision.bridgeStallCount, std::memory_order_relaxed);
    telemetryFlags_.store(flags, std::memory_order_relaxed);
    telemetryDecisions_.fetch_add(1, std::memory_order_release);
    return result;
}

MotuSphServoRuntimeSnapshot MotuV3TxTimingStamper::ServoTelemetrySnapshot() const noexcept {
    MotuSphServoRuntimeSnapshot snapshot{};
    snapshot.decisions = telemetryDecisions_.load(std::memory_order_acquire);
    snapshot.hardResyncRequests = telemetryHardResyncRequests_.load(std::memory_order_relaxed);
    snapshot.phaseRepairs = telemetryPhaseRepairs_.load(std::memory_order_relaxed);
    snapshot.muteTransitions = telemetryMuteTransitions_.load(std::memory_order_relaxed);
    snapshot.unmuteTransitions = telemetryUnmuteTransitions_.load(std::memory_order_relaxed);
    snapshot.bridgeUpdates = telemetryBridgeUpdates_.load(std::memory_order_relaxed);
    snapshot.streamGeneration = telemetryStreamGeneration_.load(std::memory_order_relaxed);
    snapshot.rxFrames = telemetryRxFrames_.load(std::memory_order_relaxed);
    snapshot.rxTicks = telemetryRxTicks_.load(std::memory_order_relaxed);
    snapshot.txCorrectionQ32 = telemetryTxCorrectionQ32_.load(std::memory_order_relaxed);
    snapshot.stepQ32 = telemetryStepQ32_.load(std::memory_order_relaxed);
    snapshot.measuredStepQ32 = telemetryMeasuredStepQ32_.load(std::memory_order_relaxed);
    snapshot.phaseErrorTicks = telemetryPhaseErrorTicks_.load(std::memory_order_relaxed);
    const uint32_t flags = telemetryFlags_.load(std::memory_order_relaxed);
    snapshot.configured = (flags & kServoFlagConfigured) != 0;
    snapshot.feedbackUpdated = (flags & kServoFlagFeedbackUpdated) != 0;
    snapshot.phaseReferenceReset = (flags & kServoFlagPhaseReferenceReset) != 0;
    snapshot.stepClamped = (flags & kServoFlagStepClamped) != 0;
    snapshot.hardResyncRequired = (flags & kServoFlagHardResyncRequired) != 0;
    snapshot.phaseRepairApplied = (flags & kServoFlagPhaseRepairApplied) != 0;
    snapshot.outputMuted = (flags & kServoFlagOutputMuted) != 0;
    snapshot.locked = (flags & kServoFlagLocked) != 0;
    snapshot.bridgeStalled = (flags & kServoFlagBridgeStalled) != 0;
    snapshot.referenceCause = static_cast<MOTU::MotuSphReferenceCause>(
        telemetryReferenceCause_.load(std::memory_order_relaxed));
    snapshot.seedReReferences = telemetrySeedReReferences_.load(std::memory_order_relaxed);
    snapshot.rxFrameRegressions = telemetryRxFrameRegressions_.load(std::memory_order_relaxed);
    snapshot.rxTickRegressions = telemetryRxTickRegressions_.load(std::memory_order_relaxed);
    snapshot.nonAdvancingTicks = telemetryNonAdvancingTicks_.load(std::memory_order_relaxed);
    snapshot.bridgeStalls = telemetryBridgeStalls_.load(std::memory_order_relaxed);
    snapshot.observationDrops = telemetryObservationDrops_.load(std::memory_order_relaxed);
    snapshot.dropBridgeNotAdvanced =
        telemetryDropBridgeNotAdvanced_.load(std::memory_order_relaxed);
    snapshot.dropBridgeRegressed = telemetryDropBridgeRegressed_.load(std::memory_order_relaxed);
    snapshot.dropRxFramesNotAdvanced =
        telemetryDropRxFramesNotAdvanced_.load(std::memory_order_relaxed);
    snapshot.dropRxFramesRegressed =
        telemetryDropRxFramesRegressed_.load(std::memory_order_relaxed);
    snapshot.dropRxTicksNotAdvanced =
        telemetryDropRxTicksNotAdvanced_.load(std::memory_order_relaxed);
    snapshot.dropRxTicksRegressed = telemetryDropRxTicksRegressed_.load(std::memory_order_relaxed);
    return snapshot;
}

} // namespace ASFW::Audio::Wire
