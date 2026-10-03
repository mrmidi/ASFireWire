// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuV3TxTimingStamper.hpp - transmit SPH timing for MOTU protocol-v3 (828 Mk3).
//
// V2 replays the device's own per-block presentation offsets (MotuTxTimingStamper).
// V3 does not: its transmit SPH is a free-running fractional clock steered by a PI
// servo against the device's receive SPH. This class implements that clock
// behind the generic ITxDeviceTimingStamper seam:
//
//  - StampPacket() writes one SPH per data block from MotuSphQ32Accumulator,
//    seeded once per start from the first DATA packet's transmit time plus the
//    presentation lead, then advancing one sample period per block;
//  - ApplyServoInput() is the servo: feed-forward from the RX SPH rate, phase
//    feedback, the hard-resync outlet that repairs the accumulator's phase once
//    per crossing, and the mute gate that holds PCM silent while unlocked.
//
// Two rules follow from the V3 wire contract.
//
// StampPacket() never returns kTimingUnavailable. The packetizer reverts such a
// packet to NO-DATA, which on V3 would change the packet cadence the device
// sees. A packet the clock cannot time yet goes out as DATA with SPH zero,
// which the device accepts.
//
// The servo is fed before EVERY packet, NO-DATA included, by the producer
// (ASFWAudioDriverTxProducer.cpp), not from StampPacket(), which runs for DATA
// only and after the packetizer. Moving the input would change the call rate of
// the observation drop gate below, whose behaviour was measured on hardware at
// this rate.
//
// Threading: the producer is the only caller of StampPacket(), ApplyServoInput()
// and ResetForStart(), serialized on the TX preparation queue. The telemetry
// snapshot is read from elsewhere, hence the atomics.

#pragma once

#include "../../DriverKit/Config/AudioStreamProfile.hpp"
#include "../../Ports/IWirePayloadCodec.hpp"
#include "../../Protocols/MOTU/MotuSphClockServo.hpp"
#include "../../Protocols/MOTU/MotuSphHardResyncGate.hpp"
#include "../AMDTP/MotuSphQ32Accumulator.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Wire {

// Plant identification, and the ONLY line an operator flips for it.
//
// Zero ships. A build with a non-zero value here is an instrument, not a
// driver: it deliberately steps the presentation phase once, mid-stream, so a
// hardware run can see whether `rel` follows the actuator and in which
// direction. Keep it well under the 12288-tick hard-resync threshold -- 1024
// (two audio frames, one cadence quantum) is the intended step, and
// MotuSphClockServo::Configure() refuses anything that could
// open the valve on its own. It lives in the header so the ring record can
// print what was armed rather than the reader having to trust the build.
inline constexpr int64_t kMotuSphPhaseInjectionTicks = 0;
// Far enough into the stream that the loop has settled and the run has a clean
// "before" median: 30 s at 48 kHz. Only meaningful when the value above is set.
inline constexpr uint64_t kMotuSphPhaseInjectionAfterFrames = 48000ULL * 30;

struct MotuSphServoRuntimeInput final {
    bool valid{false};
    uint64_t streamGeneration{0};
    uint64_t expectedStreamGeneration{0};
    uint64_t bridgeUpdates{0};
    uint64_t rxFrames{0};
    int64_t rxTicks{0};

    // The conditioned `rel` phase, from its own latest-value bridge.
    // Optional -- false here is the historical behaviour, where the loop's error
    // is whatever accumulated since the reference and a stream that started
    // offset is declared perfect forever.
    bool haveRelPhase{false};
    int64_t relPhaseTicks{0};
    int64_t relPhaseCenterTicks{0};
};

// Why ApplyServoInput() threw an observation away before the servo
// ever saw it -- the fourth swallow point on the path to the hard-resync
// valve, and the last one that was unattributed.
//
// A bitmask rather than a single cause, because the three counters are tested
// with one `||` and more than one of them can fail on the same observation;
// collapsing that to a first-hit cause would invent a precedence the code does
// not have. Each quantity contributes two mutually exclusive bits: `NotAdvanced`
// means it repeated its previous value, `Regressed` means it went backwards.
// That split is the whole point. This path runs far more often than the RX
// bridge publishes, so a repeat is the expected, benign case; a counter moving
// backwards is the anomaly worth reporting. One number covering
// both would be dominated by the benign case and would answer nothing.
inline constexpr uint8_t kMotuSphDropBridgeNotAdvanced = 1u << 0;
inline constexpr uint8_t kMotuSphDropBridgeRegressed = 1u << 1;
inline constexpr uint8_t kMotuSphDropRxFramesNotAdvanced = 1u << 2;
inline constexpr uint8_t kMotuSphDropRxFramesRegressed = 1u << 3;
inline constexpr uint8_t kMotuSphDropRxTicksNotAdvanced = 1u << 4;
inline constexpr uint8_t kMotuSphDropRxTicksRegressed = 1u << 5;

struct MotuSphServoApplyResult final {
    bool observationApplied{false};
    bool stepApplied{false};
    bool muteActivated{false};
    bool phaseRepairApplied{false};
    bool unmuteActivated{false};
    bool outputMuted{false};
    // Zero whenever the observation reached the servo, and zero as well for the
    // drops this gate does not own (unconfigured stamper, invalid sample, wrong
    // generation, or the by-design decision-cadence gate below it).
    uint8_t dropCauses{0};
    ::ASFW::Audio::MOTU::MotuSphServoDecision decision{};
};

// A hard-resync marker is an edge, not a recurring state report.  Both edges
// below advance their corresponding sticky telemetry counter; holding mute
// after the repair must not create another marker on the TX hot path.
[[nodiscard]] constexpr bool MotuSphHardResyncEventOccurred(
    const MotuSphServoApplyResult& result) noexcept {
    return result.muteActivated || result.unmuteActivated;
}

struct MotuSphServoRuntimeSnapshot final {
    uint64_t decisions{0};
    uint64_t hardResyncRequests{0};
    uint64_t phaseRepairs{0};
    uint64_t muteTransitions{0};
    uint64_t unmuteTransitions{0};
    uint64_t bridgeUpdates{0};
    uint64_t streamGeneration{0};
    uint64_t rxFrames{0};
    int64_t rxTicks{0};
    int64_t txCorrectionQ32{0};
    int64_t stepQ32{0};
    int64_t measuredStepQ32{0};
    int64_t phaseErrorTicks{0};
    bool configured{false};
    bool feedbackUpdated{false};
    bool phaseReferenceReset{false};
    bool stepClamped{false};
    bool hardResyncRequired{false};
    bool phaseRepairApplied{false};
    bool outputMuted{false};
    // Which stage of the two-stage loop the last decision came from. Redundant
    // with `outputMuted` by construction -- muted means not locked -- and
    // reported anyway, because the run that has to show the acquisition window
    // closing reads both from the same line.
    bool locked{false};

    // Where an event could have been swallowed before reaching the valve.
    // `referenceCause` attributes the last re-reference; the counts say how
    // often each cause fired since the servo was configured.
    ::ASFW::Audio::MOTU::MotuSphReferenceCause referenceCause{
        ::ASFW::Audio::MOTU::MotuSphReferenceCause::kNone};
    bool bridgeStalled{false};
    uint32_t seedReReferences{0};
    uint32_t rxFrameRegressions{0};
    uint32_t rxTickRegressions{0};
    uint32_t nonAdvancingTicks{0};
    uint32_t bridgeStalls{0};

    // The stamper-side gate, one layer above the servo. Distinct from the
    // five counts above, which the servo raises about samples it did receive:
    // these count samples it never saw. `observationDrops` is the number of
    // dropped observations; the six below attribute them and may sum to more,
    // because one observation can fail on several counters at once. Reset per
    // stream start, like `decisions`.
    uint32_t observationDrops{0};
    uint32_t dropBridgeNotAdvanced{0};
    uint32_t dropBridgeRegressed{0};
    uint32_t dropRxFramesNotAdvanced{0};
    uint32_t dropRxFramesRegressed{0};
    uint32_t dropRxTicksNotAdvanced{0};
    uint32_t dropRxTicksRegressed{0};
};

class MotuV3TxTimingStamper final : public ::ASFW::Audio::ITxDeviceTimingStamper {
public:
    /// Rejects every geometry the servo has no measured policy for: 48 kHz at
    /// eight frames per data packet only.
    [[nodiscard]] bool Configure(const ::ASFW::Isoch::Audio::AudioStreamConfig& txConfig) noexcept;

    /// Clears the clock, the servo and the per-start counters, and arms the
    /// acquisition mute.
    void ResetForStart() noexcept;

    ::ASFW::Audio::TxTimingStampResult StampPacket(
        const Protocols::Audio::AMDTP::TxPacketSlotView& slot,
        const Protocols::Audio::AMDTP::PreparedTxPacket& packet,
        const Protocols::Audio::AMDTP::AmdtpTimingState& timing) noexcept override;

    [[nodiscard]] bool IsSytUnaware() const noexcept override { return true; }

    // Consume the latest exact RX clock sample and apply one continuous
    // feed-forward + phase-feedback decision to the SPH clock. Invalid, stale
    // and duplicate samples are observational no-ops. The same decision's
    // hard-resync outlet mutes PCM, repairs the clock's phase once per
    // threshold crossing, and unmutes only inside the safe band.
    [[nodiscard]] MotuSphServoApplyResult
    ApplyServoInput(const MotuSphServoRuntimeInput& input) noexcept;

    [[nodiscard]] MotuSphServoRuntimeSnapshot ServoTelemetrySnapshot() const noexcept;

    /// The payload writer's mute: true from ResetForStart() until the loop locks,
    /// and again while a hard resync is being repaired.
    [[nodiscard]] bool IsOutputMuted() const noexcept { return gate_.IsMuted(); }

    [[nodiscard]] bool IsConfigured() const noexcept { return servo_.IsConfigured(); }
    [[nodiscard]] int64_t StepQ32() const noexcept { return clock_.StepQ32(); }
    [[nodiscard]] int64_t AppliedCorrectionQ32() const noexcept {
        return clock_.AppliedCorrectionQ32();
    }
    /// SPH of the first block of the last packet stamped, 0 before the first.
    [[nodiscard]] uint32_t LastFirstSph() const noexcept { return lastFirstSph_; }

private:
    // Classify one rejected observation against the previous sample and record
    // it. Returns the mask, zero meaning the observation may proceed.
    [[nodiscard]] uint8_t ClassifyAndCountDrop(const MotuSphServoRuntimeInput& input) noexcept;

    ::ASFW::Protocols::Audio::AMDTP::MotuSphQ32Accumulator clock_{};
    ::ASFW::Audio::MOTU::MotuSphClockServo servo_{};
    ::ASFW::Audio::MOTU::MotuSphHardResyncGate gate_{};
    uint32_t lastFirstSph_{0};

    uint64_t streamGeneration_{0};
    uint64_t lastBridgeUpdates_{0};
    uint64_t lastSeenRxFrames_{0};
    int64_t lastSeenRxTicks_{0};
    uint64_t lastAppliedRxFrames_{0};
    uint64_t updateIntervalFrames_{0};
    // Whether any observation of this generation has carried absolute phase.
    // The transition to true is what re-references the loop: the reference is
    // built ~30 ms into the stream, long before the conditioner has warmed up.
    bool haveRelPhase_{false};

    std::atomic<uint64_t> telemetryDecisions_{0};
    std::atomic<uint64_t> telemetryHardResyncRequests_{0};
    std::atomic<uint64_t> telemetryPhaseRepairs_{0};
    std::atomic<uint64_t> telemetryMuteTransitions_{0};
    std::atomic<uint64_t> telemetryUnmuteTransitions_{0};
    std::atomic<uint64_t> telemetryBridgeUpdates_{0};
    std::atomic<uint64_t> telemetryStreamGeneration_{0};
    std::atomic<uint64_t> telemetryRxFrames_{0};
    std::atomic<int64_t> telemetryRxTicks_{0};
    std::atomic<int64_t> telemetryTxCorrectionQ32_{0};
    std::atomic<int64_t> telemetryStepQ32_{0};
    std::atomic<int64_t> telemetryMeasuredStepQ32_{0};
    std::atomic<int64_t> telemetryPhaseErrorTicks_{0};
    std::atomic<uint32_t> telemetryFlags_{0};
    // Counts are produced by the servo, which is the only place that
    // classifies a cause; these just carry them off the TX hot path.
    std::atomic<uint32_t> telemetryReferenceCause_{0};
    std::atomic<uint32_t> telemetrySeedReReferences_{0};
    std::atomic<uint32_t> telemetryRxFrameRegressions_{0};
    std::atomic<uint32_t> telemetryRxTickRegressions_{0};
    std::atomic<uint32_t> telemetryNonAdvancingTicks_{0};
    std::atomic<uint32_t> telemetryBridgeStalls_{0};

    // Stamper-owned, so nothing in the servo mirrors them.
    std::atomic<uint32_t> telemetryObservationDrops_{0};
    std::atomic<uint32_t> telemetryDropBridgeNotAdvanced_{0};
    std::atomic<uint32_t> telemetryDropBridgeRegressed_{0};
    std::atomic<uint32_t> telemetryDropRxFramesNotAdvanced_{0};
    std::atomic<uint32_t> telemetryDropRxFramesRegressed_{0};
    std::atomic<uint32_t> telemetryDropRxTicksNotAdvanced_{0};
    std::atomic<uint32_t> telemetryDropRxTicksRegressed_{0};
};

} // namespace ASFW::Audio::Wire
