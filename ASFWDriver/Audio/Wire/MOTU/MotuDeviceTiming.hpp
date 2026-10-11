// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Ports/IWirePayloadCodec.hpp"
#include "MotuEventOffsetCache.hpp"
#include "MotuTxTiming.hpp"
#include "MotuModel.hpp"
#include "MotuSphClockServo.hpp"
#include "MotuSphHardResyncGate.hpp"
#include "MotuSphQ32Accumulator.hpp"
#include "MotuSphServoPolicy.hpp"
#include "../../DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../../DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "../../DriverKit/Runtime/MotuRxSphRateMeter.hpp"
#include "../../DriverKit/Runtime/MotuServoStallDetector.hpp"
#include "../../../Logging/Logging.hpp"

#include <atomic>
#include <cstdint>
#include <span>

#include "MotuRxDiagnosticCapture.hpp"

namespace ASFW::Audio::Wire {

// Besides the per-block offset cache, the observer measures the device's
// sample period from the received SPH (MotuRxSphRateMeter) and publishes the
// cumulative measurement over the control block's motuRxSphClock bridge, for
// the transmit SPH regulator. The SPH format is the same for every MOTU
// protocol version, so this runs for all of them.
//
// Stream boundaries: a new control generation (StartIO) starts the measurement
// over, together with the bridge. Reset() -- receive activation, quiesce and
// replay discontinuities -- stays inside the stream: it re-anchors the meter so
// the step across the gap is not measured, and keeps the cumulative totals so
// the bridge stays monotonic.
class MotuRxTimingObserver final : public IRxDeviceTimingObserver {
public:
    MotuRxTimingObserver() noexcept = default;

    explicit MotuRxTimingObserver(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept
        : bindingSource_(bindingSource) {}

    explicit MotuRxTimingObserver(
        ::ASFW::Encoding::Motu::MotuEventOffsetCache* cache) noexcept
        : explicitCache_(cache), activeCache_(cache) {}

    void BindSource(::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept {
        bindingSource_ = bindingSource;
    }

    void BindCache(::ASFW::Encoding::Motu::MotuEventOffsetCache* cache) noexcept {
        explicitCache_ = cache;
        activeCache_ = cache;
    }

    void BindDiagnosticCapture(MotuRxDiagnosticCapture* capture, uint32_t strideQuadlets = 0, uint64_t sourceGuid = 0) noexcept {
        diagnosticCapture_ = capture;
        diagnosticStride_ = strideQuadlets;
        diagnosticGuid_ = sourceGuid;
    }

    void OnBatchBegin(::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept override {
        if (bindingSource != nullptr) {
            bindingSource_ = bindingSource;
        }
        if (explicitCache_ != nullptr) {
            activeCache_ = explicitCache_;
            return;
        }
        if (bindingSource_ != nullptr) {
            ::ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
            if (bindingSource_->CopyDirectAudioBinding(snapshot) && snapshot.control != nullptr) {
                activeCache_ = &snapshot.control->motuEventOffsets;
                sampleRateHz_ = snapshot.sampleRateHz;
                BindClockControl(snapshot.control);
            } else {
                activeCache_ = nullptr;
                sampleRateHz_ = 0;
                clockControl_ = nullptr;
            }
        }
    }

    void ObserveRawPacket(
        uint64_t epoch,
        uint16_t rxTimestamp,
        std::span<const uint8_t> rawPayload) noexcept override {
        if (diagnosticCapture_ != nullptr) {
            diagnosticCapture_->RecordPacket(epoch, rxTimestamp, rawPayload, diagnosticStride_, kMotuCipPrefixBytes, diagnosticGuid_, sampleRateHz_);
        }
    }

    void ObservePacket(
        std::span<const uint8_t> payload,
        uint32_t strideQuadlets,
        uint32_t framesDecoded,
        uint16_t cycle) noexcept override {
        MeasureRxClock(payload, framesDecoded);
        if (activeCache_ == nullptr) {
            return;
        }
        const uint32_t cached = activeCache_->Capture(
            payload, strideQuadlets, framesDecoded, cycle, kMotuCipPrefixBytes);
        if (cached > 0) {
            timingEstablished_ = true;
        }
    }

    [[nodiscard]] bool IsTimingEstablished() const noexcept override {
        return timingEstablished_ && (activeCache_ != nullptr && activeCache_->IsEstablished());
    }

    void Reset() noexcept override {
        timingEstablished_ = false;
        if (activeCache_ != nullptr) {
            activeCache_->Reset();
        }
        rxClock_.Reanchor();
        stallDetector_.Reset();
        // Reset() also runs on quiesce, after which the control block may be
        // unbound. Drop the view; the next OnBatchBegin re-binds it.
        clockControl_ = nullptr;
    }

    // Off the hot path, on the consumer's telemetry cadence.
    void DrainTelemetry(uint32_t maxRecords) noexcept override {
        (void)maxRecords;
        CheckRxClockStall();
    }

    // Times the measurement was found behind the bridge, since construction.
    [[nodiscard]] uint32_t RxClockStalls() const noexcept { return rxClockStalls_; }

private:
    // 8-byte isochronous receive prefix plus the 8-byte CIP header.
    static constexpr uint32_t kMotuCipPrefixBytes = 16;

    // StartIO advances the control generation, possibly reusing the same
    // mapped control block, so the stream boundary is the generation, not
    // the binding.
    void BindClockControl(::ASFW::Audio::Runtime::AudioTransportControlBlock* control) noexcept {
        const uint64_t generation = control->generation.load(std::memory_order_acquire);
        if (control != clockGenerationControl_ || generation != clockGeneration_) {
            rxClock_.Reset();
            stallDetector_.Reset();
            clockGenerationControl_ = control;
            clockGeneration_ = generation;
        }
        clockControl_ = control;
    }

    // The first block's SPH and the packet's frame count; NO-DATA packets
    // carry neither and are skipped by the meter.
    void MeasureRxClock(std::span<const uint8_t> payload, uint32_t framesDecoded) noexcept {
        if (framesDecoded == 0 || payload.size() < kMotuCipPrefixBytes + 4U) {
            return;
        }
        const uint32_t sph = ::ASFW::Encoding::Motu::ReadSph(payload.subspan(kMotuCipPrefixBytes, 4));
        rxClock_.Observe(sph, framesDecoded, sampleRateHz_);
        if (clockControl_ == nullptr) {
            return;
        }
        const auto cumulative = rxClock_.CumulativeSnapshot();
        if (!cumulative.valid) {
            return;
        }
        // Refused unless both totals moved forward, e.g. on the packet that
        // re-anchors the meter: nothing new to publish then.
        (void)clockControl_->motuRxSphClock.Publish({
            .streamGeneration = clockControl_->generation.load(std::memory_order_acquire),
            .rxFrames = cumulative.frames,
            .rxTicks = cumulative.ticks,
        });
    }

    void CheckRxClockStall() noexcept {
        auto* control = clockControl_;
        if (control == nullptr) {
            return;
        }
        ::ASFW::Audio::Runtime::MotuRxSphClockSample bridge{};
        uint64_t bridgeUpdates = 0;
        if (!control->motuRxSphClock.ReadLatest(bridge, bridgeUpdates)) {
            return;
        }
        ::ASFW::Audio::Runtime::MotuServoStallEvent event{};
        if (!stallDetector_.Observe(control->generation.load(std::memory_order_acquire),
                                    rxClock_.CumulativeSnapshot(), bridge, bridgeUpdates, event)) {
            return;
        }
        ++rxClockStalls_;
        ASFW_LOG(DirectAudio,
                 "[MotuSphStall] reason=rx-meter-behind-bridge gen=%llu updates=%llu "
                 "bridgeFrames=%llu bridgeTicks=%lld meterFrames=%llu meterTicks=%lld",
                 event.streamGeneration, event.bridgeUpdates, event.bridgeFrames,
                 static_cast<long long>(event.bridgeTicks), event.meterFrames,
                 static_cast<long long>(event.meterTicks));
    }

    ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource_{nullptr};
    ::ASFW::Encoding::Motu::MotuEventOffsetCache* explicitCache_{nullptr};
    ::ASFW::Encoding::Motu::MotuEventOffsetCache* activeCache_{nullptr};
    MotuRxDiagnosticCapture* diagnosticCapture_{nullptr};
    uint32_t sampleRateHz_{0};
    uint64_t diagnosticGuid_{0};
    uint32_t diagnosticStride_{0};
    bool timingEstablished_{false};

    // The receive poll and the telemetry drain are serialized on the same
    // dispatch queue, so these are plain state.
    ::ASFW::Audio::Runtime::MotuRxSphRateMeter rxClock_{};
    ::ASFW::Audio::Runtime::MotuServoStallDetector stallDetector_{};
    ::ASFW::Audio::Runtime::AudioTransportControlBlock* clockControl_{nullptr};
    // Compared only, never dereferenced: identifies the stream the meter's
    // totals belong to.
    const ::ASFW::Audio::Runtime::AudioTransportControlBlock* clockGenerationControl_{nullptr};
    uint64_t clockGeneration_{0};
    uint32_t rxClockStalls_{0};
};

// ---------------------------------------------------------------------------
// Transmit: the SPH regulator.
//
// MOTU carries presentation time in the SPH quadlet at the head of every data
// block (for every protocol version; the CIP SYT is NO_INFO). The transmit SPH
// is a free-running clock (MotuSphQ32Accumulator): seeded once per start from
// the first DATA packet's transmit time plus the model's presentation lead,
// then advanced one sample period per stamped block. The regulator
// (MotuSphClockServo) sets that period from the device's own sample period,
// measured from received SPH and handed over by the RX timing observer
// (motuRxSphClock), plus a phase feedback term. Above a four-cycle error it
// asks for a one-shot phase repair instead (MotuSphHardResyncGate).
//
// The packet cadence -- which cycles carry DATA and how many blocks -- still
// replays the device's receive stream; the regulator only times the blocks.
//
// The servo is fed by the transmit producer before EVERY packet, NO-DATA
// included (ASFWAudioDriverTxProducer.cpp), not from StampPacket(), which runs
// for DATA packets only. Its decisions are rate-limited to one per decision
// interval of received frames (MotuSphServoPolicy.hpp), so the call rate only
// decides how soon a new RX sample is seen.

// One RX clock observation as the producer hands it over: the latest value of
// the control block's motuRxSphClock bridge, and the generation the control
// block is in now.
struct MotuSphServoRuntimeInput final {
    bool valid{false};
    uint64_t streamGeneration{0};
    uint64_t expectedStreamGeneration{0};
    uint64_t bridgeUpdates{0};
    uint64_t rxFrames{0};
    int64_t rxTicks{0};

    // The conditioned absolute phase of our stream against the device, from
    // its own bridge. Optional: without it the loop is relative and holds the
    // phase the stream started with.
    bool haveRelPhase{false};
    int64_t relPhaseTicks{0};
    int64_t relPhaseCenterTicks{0};
};

// Why ApplyServoInput() dropped an observation before the servo saw it. A
// bitmask, because one observation can fail on several counters at once. Each
// counter has two exclusive bits: `NotAdvanced` (it repeated its value, the
// expected case -- the producer runs far more often than RX publishes) and
// `Regressed` (it went backwards, the anomaly worth reporting).
inline constexpr uint8_t kMotuSphDropBridgeNotAdvanced = 1u << 0;
inline constexpr uint8_t kMotuSphDropBridgeRegressed = 1u << 1;
inline constexpr uint8_t kMotuSphDropRxFramesNotAdvanced = 1u << 2;
inline constexpr uint8_t kMotuSphDropRxFramesRegressed = 1u << 3;
inline constexpr uint8_t kMotuSphDropRxTicksNotAdvanced = 1u << 4;
inline constexpr uint8_t kMotuSphDropRxTicksRegressed = 1u << 5;

struct MotuSphServoApplyResult final {
    bool observationApplied{false};
    bool stepApplied{false};
    // A hard resync muted the output ahead of its phase repair.
    bool muteActivated{false};
    // A hard-resync crossing began on this decision; the one-shot phase
    // repair was requested.
    bool hardResyncStarted{false};
    bool phaseRepairApplied{false};
    // The loop locked and the output became audible.
    bool unmuteActivated{false};
    // A new stream generation started a fresh, muted acquisition.
    bool acquisitionRestarted{false};
    bool outputMuted{false};
    // Zero whenever the observation reached the servo, and for the drops this
    // gate does not own (unconfigured stamper, invalid sample, wrong
    // generation, or the decision-interval gate).
    uint8_t dropCauses{0};
    ::ASFW::Encoding::Motu::MotuSphServoDecision decision{};
};

// A hard-resync event is an edge: the mute before the repair, or the unmute
// after it. Holding the mute is not an event.
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
    int64_t nominalStepQ32{0};
    int64_t measuredStepQ32{0};
    int64_t phaseErrorTicks{0};
    bool configured{false};
    bool seeded{false};
    bool feedbackUpdated{false};
    bool phaseReferenceReset{false};
    bool stepClamped{false};
    bool hardResyncRequired{false};
    bool phaseRepairApplied{false};
    bool outputMuted{false};
    bool locked{false};

    // Re-references and stalls the servo reported about samples it received.
    ::ASFW::Encoding::Motu::MotuSphReferenceCause referenceCause{
        ::ASFW::Encoding::Motu::MotuSphReferenceCause::kNone};
    bool bridgeStalled{false};
    uint32_t seedReReferences{0};
    uint32_t rxFrameRegressions{0};
    uint32_t rxTickRegressions{0};
    uint32_t nonAdvancingTicks{0};
    uint32_t bridgeStalls{0};

    // Samples the servo never saw (kMotuSphDrop*). `observationDrops` counts
    // drops; the six below attribute them and may sum to more. Per start.
    uint32_t observationDrops{0};
    uint32_t dropBridgeNotAdvanced{0};
    uint32_t dropBridgeRegressed{0};
    uint32_t dropRxFramesNotAdvanced{0};
    uint32_t dropRxFramesRegressed{0};
    uint32_t dropRxTicksNotAdvanced{0};
    uint32_t dropRxTicksRegressed{0};
};

// A step's sample rate against nominal, in parts per billion, for telemetry:
// positive when the step is shorter than nominal, i.e. the clock runs fast.
[[nodiscard]] constexpr int64_t MotuSphRatePpb(int64_t stepQ32, int64_t nominalStepQ32) noexcept {
    if (stepQ32 <= 0 || nominalStepQ32 <= 0) {
        return 0;
    }
    return static_cast<int64_t>(
        (static_cast<__int128>(nominalStepQ32 - stepQ32) * 1'000'000'000) / stepQ32);
}

// Threading: the transmit producer calls StampPacket(), ApplyServoInput() and
// ServoTelemetrySnapshot() on the transmit preparation queue. Configure() and
// ResetForStart() run when the stream is armed, before that queue prepares
// anything. The readiness word is the only state another thread reads.
class MotuTxTimingStamper final : public ::ASFW::Audio::ITxDeviceTimingStamper {
public:
    MotuTxTimingStamper() noexcept = default;

    // Refuses a rate the regulator has no configuration for, and leaves the
    // stamper unconfigured then.
    [[nodiscard]] bool Configure(uint32_t dbs, uint32_t sampleRateHz,
                                 uint32_t presentationLeadTicks) noexcept {
        dbs_ = dbs;
        presentationLeadTicks_ = presentationLeadTicks;
        const auto config = ::ASFW::Encoding::Motu::MotuSphServoConfigForRate(sampleRateHz);
        const bool configured = config.has_value() && clock_.Configure(sampleRateHz) &&
                                servo_.Configure(*config);
        if (!configured) {
            (void)clock_.Configure(0);
            (void)servo_.Configure({});
        }
        decisionIntervalFrames_ = configured ? config->phaseCorrectionHorizonFrames : 0;
        ResetForStart();
        return configured;
    }

    // Clears the clock, the servo and the per-start counters, and arms the
    // acquisition mute.
    void ResetForStart() noexcept {
        clock_.Reset();
        servo_.Reset();
        lastFirstSph_ = 0;
        // Armed only where a servo will produce the decisions that lift it: a
        // latch nobody clears would be permanent silence.
        const bool acquisitionMute = servo_.IsConfigured();
        gate_.Reset(acquisitionMute);
        streamGeneration_ = 0;
        lastBridgeUpdates_ = 0;
        lastSeenRxFrames_ = 0;
        lastSeenRxTicks_ = 0;
        lastAppliedRxFrames_ = 0;
        haveRelPhase_ = false;
        telemetry_ = {};
        // The acquisition mute is a mute the stream really has, so it counts:
        // a clean start reads one mute and one unmute.
        telemetry_.muteTransitions = acquisitionMute ? 1 : 0;
        telemetry_.stepQ32 = clock_.StepQ32();
        telemetry_.measuredStepQ32 = clock_.StepQ32();
        PublishReady(false);
    }

    // Where the session reads "presentation timing acquired" before it lets
    // the device fetch (MotuProtocol, requireTimingBeforeFetch). Null unbinds.
    void BindReadiness(std::atomic<uint32_t>* readiness) noexcept { readiness_ = readiness; }

    ::ASFW::Audio::TxTimingStampResult StampPacket(
        const Protocols::Audio::AMDTP::TxPacketSlotView& slot,
        const Protocols::Audio::AMDTP::PreparedTxPacket& packet,
        const Protocols::Audio::AMDTP::AmdtpTimingState& timing) noexcept override {
        const uint32_t blocks = packet.framesInPacket;
        if (slot.bytes == nullptr || dbs_ == 0 || blocks == 0) {
            return ::ASFW::Audio::TxTimingStampResult::kNotApplicable;
        }
        auto payload = std::span<uint8_t>(slot.bytes, packet.byteCount);
        const uint64_t blockBytes = uint64_t{dbs_} * 4U;
        const auto unavailable = [&]() noexcept {
            (void)::ASFW::Encoding::Motu::WritePacketSphZero(payload, dbs_, blocks,
                                                             kCipHeaderBytes);
            return ::ASFW::Audio::TxTimingStampResult::kTimingUnavailable;
        };
        // A packet that cannot hold every block is a geometry fault: stamp
        // nothing, so the clock does not move for blocks never sent.
        if (payload.size() < kCipHeaderBytes + blockBytes * blocks) {
            return unavailable();
        }

        // The first DATA packet with a transmit time seeds the clock. From
        // then on the clock always has a time, so no packet is reverted to
        // NO-DATA for missing timing again. Before the seed there is no
        // presentation time to give, and the packet goes out as NO-DATA.
        if (timing.transmitCycleValid) {
            (void)clock_.SeedOnce(timing.transmitTicks + presentationLeadTicks_);
        }
        if (!clock_.IsSeeded()) {
            return unavailable();
        }

        for (uint32_t block = 0; block < blocks; ++block) {
            const uint32_t sph = clock_.CurrentSph();
            if (block == 0) {
                lastFirstSph_ = sph;
            }
            ::ASFW::Encoding::Motu::WriteSph(
                payload.subspan(static_cast<size_t>(kCipHeaderBytes + block * blockBytes), 4),
                sph);
            clock_.Advance();
        }
        return ::ASFW::Audio::TxTimingStampResult::kOk;
    }

    [[nodiscard]] bool IsSytUnaware() const noexcept override {
        return true;
    }

    // One RX clock observation. Invalid, stale and repeated samples change
    // nothing; a new one feeds at most one servo decision per decision
    // interval. The decision sets the clock's step; its hard-resync outlet
    // mutes, repairs the clock's phase once per crossing, and unmutes once the
    // loop is locked again.
    [[nodiscard]] MotuSphServoApplyResult ApplyServoInput(
        const MotuSphServoRuntimeInput& input) noexcept {
        MotuSphServoApplyResult result{};
        result.decision = servo_.Current();
        result.outputMuted = gate_.IsMuted();
        ApplyObservation(input, result);
        PublishReady(IsReady(input.expectedStreamGeneration));
        return result;
    }

    [[nodiscard]] MotuSphServoRuntimeSnapshot ServoTelemetrySnapshot() const noexcept {
        MotuSphServoRuntimeSnapshot snapshot = telemetry_;
        snapshot.configured = servo_.IsConfigured();
        snapshot.seeded = clock_.IsSeeded();
        snapshot.outputMuted = gate_.IsMuted();
        snapshot.locked = servo_.Current().locked;
        snapshot.nominalStepQ32 = clock_.NominalStepQ32();
        return snapshot;
    }

    // The payload writer's mute: from ResetForStart() until the loop locks,
    // and again while a hard resync is repaired.
    [[nodiscard]] bool IsOutputMuted() const noexcept { return gate_.IsMuted(); }

    [[nodiscard]] bool IsConfigured() const noexcept { return servo_.IsConfigured(); }
    [[nodiscard]] bool IsSeeded() const noexcept { return clock_.IsSeeded(); }
    [[nodiscard]] int64_t StepQ32() const noexcept { return clock_.StepQ32(); }
    [[nodiscard]] int64_t AppliedCorrectionQ32() const noexcept {
        return clock_.AppliedCorrectionQ32();
    }
    [[nodiscard]] uint64_t DecisionIntervalFrames() const noexcept {
        return decisionIntervalFrames_;
    }
    [[nodiscard]] uint32_t PresentationLeadTicks() const noexcept {
        return presentationLeadTicks_;
    }
    // SPH of the first block of the last packet stamped, 0 before the first.
    [[nodiscard]] uint32_t LastFirstSph() const noexcept { return lastFirstSph_; }

private:
    static constexpr uint32_t kCipHeaderBytes = 8;

    void ApplyObservation(const MotuSphServoRuntimeInput& input,
                          MotuSphServoApplyResult& result) noexcept {
        if (!servo_.IsConfigured() || !input.valid || input.streamGeneration == 0 ||
            input.streamGeneration != input.expectedStreamGeneration ||
            input.bridgeUpdates == 0 || input.rxFrames == 0 || input.rxTicks <= 0) {
            return;
        }

        const bool newGeneration = input.streamGeneration != streamGeneration_;
        if (!newGeneration) {
            // Drop unless all three counters advanced, with the cause
            // recorded: a silent drop here would make the servo's own
            // regression counters unreachable and their zeros meaningless.
            result.dropCauses = ClassifyAndCountDrop(input);
            if (result.dropCauses != 0) {
                return;
            }
        } else {
            // A new generation is a new acquisition, muted exactly like a
            // start. Counted only when it silences an audible stream: the
            // first observation of every start lands here right after
            // ResetForStart() armed the mute.
            servo_.Reset();
            const bool wasAudible = !gate_.IsMuted();
            gate_.Reset(true);
            if (wasAudible) {
                ++telemetry_.muteTransitions;
                result.acquisitionRestarted = true;
            }
            result.outputMuted = true;
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
            input.rxFrames - lastAppliedRxFrames_ < decisionIntervalFrames_) {
            return;
        }

        // The reference of a generation is taken on its first observation,
        // long before an absolute phase is available, so the arrival of the
        // first one re-references the loop.
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
            .haveAbsolutePhase = input.haveRelPhase,
            .absolutePhaseTicks = input.relPhaseTicks,
            .haveAbsolutePhaseCenter = input.haveRelPhase,
            .absolutePhaseCenterTicks = input.relPhaseCenterTicks,
            .absolutePhaseAcquired = relPhaseAcquired,
        });
        result.observationApplied = true;

        const auto action = gate_.Update(result.decision.hardResyncRequired, result.decision.locked);
        result.muteActivated = action.muteBeforeRepair;
        result.unmuteActivated = action.unmute;
        result.outputMuted = action.muted;
        result.hardResyncStarted = action.phaseRepairRequired;
        if (action.phaseRepairRequired) {
            ++telemetry_.hardResyncRequests;
            result.phaseRepairApplied = clock_.ApplyPhaseCorrectionQ32(result.decision.phaseErrorQ32);
            if (result.phaseRepairApplied) {
                ++telemetry_.phaseRepairs;
            }
        }
        if (result.muteActivated) {
            ++telemetry_.muteTransitions;
        }
        if (result.unmuteActivated) {
            ++telemetry_.unmuteTransitions;
        }
        result.stepApplied = result.decision.valid && clock_.SetStepQ32(result.decision.stepQ32);
        lastAppliedRxFrames_ = input.rxFrames;

        const auto& decision = result.decision;
        ++telemetry_.decisions;
        telemetry_.bridgeUpdates = input.bridgeUpdates;
        telemetry_.streamGeneration = input.streamGeneration;
        telemetry_.rxFrames = input.rxFrames;
        telemetry_.rxTicks = input.rxTicks;
        telemetry_.txCorrectionQ32 = clock_.AppliedCorrectionQ32();
        telemetry_.stepQ32 = decision.stepQ32;
        telemetry_.measuredStepQ32 = decision.measuredStepQ32;
        telemetry_.phaseErrorTicks = decision.phaseErrorTicks;
        telemetry_.feedbackUpdated = decision.feedbackUpdated;
        telemetry_.phaseReferenceReset = decision.phaseReferenceReset;
        telemetry_.stepClamped = decision.stepClamped;
        telemetry_.hardResyncRequired = decision.hardResyncRequired;
        telemetry_.phaseRepairApplied = result.phaseRepairApplied;
        telemetry_.referenceCause = decision.referenceCause;
        telemetry_.bridgeStalled = decision.bridgeStalled;
        telemetry_.seedReReferences = decision.seedReReferenceCount;
        telemetry_.rxFrameRegressions = decision.rxFrameRegressionCount;
        telemetry_.rxTickRegressions = decision.rxTickRegressionCount;
        telemetry_.nonAdvancingTicks = decision.nonAdvancingTickCount;
        telemetry_.bridgeStalls = decision.bridgeStallCount;
    }

    // Classify one rejected observation against the previous sample and
    // count it. Zero means the observation may proceed.
    [[nodiscard]] uint8_t ClassifyAndCountDrop(const MotuSphServoRuntimeInput& input) noexcept {
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
        ++telemetry_.observationDrops;
        telemetry_.dropBridgeNotAdvanced += (causes & kMotuSphDropBridgeNotAdvanced) ? 1 : 0;
        telemetry_.dropBridgeRegressed += (causes & kMotuSphDropBridgeRegressed) ? 1 : 0;
        telemetry_.dropRxFramesNotAdvanced += (causes & kMotuSphDropRxFramesNotAdvanced) ? 1 : 0;
        telemetry_.dropRxFramesRegressed += (causes & kMotuSphDropRxFramesRegressed) ? 1 : 0;
        telemetry_.dropRxTicksNotAdvanced += (causes & kMotuSphDropRxTicksNotAdvanced) ? 1 : 0;
        telemetry_.dropRxTicksRegressed += (causes & kMotuSphDropRxTicksRegressed) ? 1 : 0;
        return causes;
    }

    // Ready for the device to fetch: the loop has locked on the stream the
    // control block is in now, and the clock times the DATA packets. A lock
    // from an earlier generation does not count -- the control block may have
    // been reset for a new start while this stamper still holds it.
    [[nodiscard]] bool IsReady(uint64_t expectedStreamGeneration) const noexcept {
        return servo_.IsConfigured() && clock_.IsSeeded() && !gate_.IsMuted() &&
               streamGeneration_ != 0 && streamGeneration_ == expectedStreamGeneration;
    }

    void PublishReady(bool ready) noexcept {
        if (readiness_ != nullptr) {
            readiness_->store(ready ? 1U : 0U, std::memory_order_release);
        }
    }

    ::ASFW::Encoding::Motu::MotuSphQ32Accumulator clock_{};
    ::ASFW::Encoding::Motu::MotuSphClockServo servo_{};
    ::ASFW::Encoding::Motu::MotuSphHardResyncGate gate_{};
    std::atomic<uint32_t>* readiness_{nullptr};
    uint32_t dbs_{0};
    uint32_t presentationLeadTicks_{::ASFW::Encoding::Motu::kDefaultPresentationLeadTicks};
    uint64_t decisionIntervalFrames_{0};
    uint32_t lastFirstSph_{0};

    uint64_t streamGeneration_{0};
    uint64_t lastBridgeUpdates_{0};
    uint64_t lastSeenRxFrames_{0};
    int64_t lastSeenRxTicks_{0};
    uint64_t lastAppliedRxFrames_{0};
    // Whether any observation of this generation carried absolute phase.
    bool haveRelPhase_{false};

    MotuSphServoRuntimeSnapshot telemetry_{};
};

} // namespace ASFW::Audio::Wire
