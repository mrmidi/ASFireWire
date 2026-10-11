// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Ports/IWirePayloadCodec.hpp"
#include "MotuTxTiming.hpp"
#include "MotuModel.hpp"
#include "MotuRelPhaseConditioner.hpp"
#include "MotuSphSynthesizer.hpp"
#include "../../DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../../DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "../../DriverKit/Runtime/MotuRxSphRateMeter.hpp"
#include "../../../Common/TimingUtils.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../../Shared/Isoch/AudioHalBufferProfiles.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>

#include "MotuRxDiagnosticCapture.hpp"

namespace ASFW::Audio::Wire {

// RX-owned raw SPH measurement and absolute-phase acquisition. Control work
// runs here on the RX queue; diagnostics may read it but never drive it.
class MotuRxTimingObserver final : public IRxDeviceTimingObserver {
public:
    MotuRxTimingObserver() noexcept = default;

    explicit MotuRxTimingObserver(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept
        : bindingSource_(bindingSource) {}

    void BindSource(::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept {
        bindingSource_ = bindingSource;
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
        if (bindingSource_ != nullptr) {
            ::ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
            if (bindingSource_->CopyDirectAudioBinding(snapshot) && snapshot.control != nullptr) {
                sampleRateHz_ = snapshot.sampleRateHz;
                BindClockControl(snapshot.control);
            } else {
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
        MeasureRxClock(payload, framesDecoded, cycle);
        if (phaseWindowDue_ && clockControl_ != nullptr) {
            ::ASFW::Audio::Runtime::MotuPhaseTraceCadenceSample trace{};
            uint64_t updates = 0;
            if (clockControl_->motuPhaseTrace.ReadLatest(trace, updates) &&
                trace.streamGeneration == clockGeneration_ &&
                ::ASFW::Audio::Runtime::ComputeMotuTxPhaseResidualCadenceMean(trace).valid) {
                phaseWindowDue_ = false;
                ClosePhaseWindow();
            }
        }
        // A DATA packet whose first block's SPH is there to read.
        if (clockControl_ != nullptr && framesDecoded != 0 && strideQuadlets != 0 &&
            payload.size() >= kMotuCipPrefixBytes + 4U) {
            timingEstablished_ = true;
        }
    }

    [[nodiscard]] bool IsTimingEstablished() const noexcept override {
        return timingEstablished_ && clockControl_ != nullptr;
    }

    void Reset() noexcept override {
        timingEstablished_ = false;
        rxClock_.Reanchor();
        RestartPhaseSampling();
        // Reset() also runs on quiesce, after which the control block may be
        // unbound. Drop the view; the next OnBatchBegin re-binds it.
        clockControl_ = nullptr;
    }

    // Control updates run from ObservePacket, independently of log verbosity.
    void DrainTelemetry(uint32_t) noexcept override {}

    // `rel` samples the motuRelPhase bridge took, and refused, this stream
    // generation.
    [[nodiscard]] uint64_t RelPhasePublished() const noexcept { return relPhasePublished_; }
    [[nodiscard]] uint64_t RelPhaseRejected() const noexcept { return relPhaseRejected_; }

    // Telemetry windows closed in total, and `rel` values formed, since
    // construction; the last `rel` formed, before conditioning.
    [[nodiscard]] uint64_t PhaseWindowsClosed() const noexcept { return phaseWindowsClosed_; }
    [[nodiscard]] uint64_t RelPhaseFormed() const noexcept { return relPhaseFormed_; }
    [[nodiscard]] int64_t LastRelTicks() const noexcept { return lastRelTicks_; }

    // Device frames per telemetry window: four seconds.
    static constexpr uint32_t kPhaseWindowSeconds = 4;

private:
    // 8-byte isochronous receive prefix plus the 8-byte CIP header.
    static constexpr uint32_t kMotuCipPrefixBytes = 16;

    // StartIO advances the control generation, possibly reusing the same
    // mapped control block, so the stream boundary is the generation, not
    // the binding.
    void BindClockControl(::ASFW::Audio::Runtime::AudioTransportControlBlock* control) noexcept {
        const uint64_t generation = control->generation.load(std::memory_order_acquire);
        if (control != clockGenerationControl_ || generation != clockGeneration_) {
            timingEstablished_ = false;
            rxClock_.Reset();
            relPhase_.Reset();
            relPhasePublished_ = 0;
            relPhaseRejected_ = 0;
            RestartPhaseSampling();
            clockGenerationControl_ = control;
            clockGeneration_ = generation;
        }
        clockControl_ = control;
    }

    void RestartPhaseSampling() noexcept {
        framesSinceReset_ = 0;
        phaseSamples_ = 0;
        nextPhaseWindowFrame_ = 0;
        phaseWindowDue_ = false;
    }

    // The first block's SPH and the packet's frame count; NO-DATA packets
    // carry neither and are skipped by the meter.
    void MeasureRxClock(std::span<const uint8_t> payload, uint32_t framesDecoded,
                        uint16_t cycle) noexcept {
        if (framesDecoded == 0 || payload.size() < kMotuCipPrefixBytes + 4U) {
            return;
        }
        const uint32_t sph = ::ASFW::Encoding::Motu::ReadSph(payload.subspan(kMotuCipPrefixBytes, 4));
        rxClock_.Observe(sph, framesDecoded, sampleRateHz_);
        SampleAbsolutePhase(sph, framesDecoded, cycle);
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

    // One absolute-phase sample per HAL zero-timestamp period, counted in
    // this observer's own frames since Reset(): the packet holding the
    // period's first frame. The period is a whole number of cadence periods,
    // so within a stream every sample lands on the same packet of the
    // cadence. The receive cycle stands for the cycle timer at offset 0; the
    // SPH lives in a one-second domain, so the cycle alone is enough.
    void SampleAbsolutePhase(uint32_t sph, uint32_t framesDecoded, uint16_t cycle) noexcept {
        const uint64_t firstFrame = framesSinceReset_;
        framesSinceReset_ += framesDecoded;
        const uint64_t period =
            ::ASFW::IsochTransport::HalBufferProfileForRate(sampleRateHz_).zeroTimestampPeriodFrames;
        if (period == 0) {
            return;
        }
        const uint64_t boundary = ((firstFrame + period - 1) / period) * period;
        if (boundary >= framesSinceReset_) {
            return;
        }
        rxClock_.ObserveAbsolutePhase(sph, ::ASFW::Timing::encodeCycleTimer(0, cycle, 0));

        // The window closes on the first sample after a Reset(), then once
        // per four seconds of device frames; ObservePacket closes it.
        const uint64_t windowFrames = uint64_t{sampleRateHz_} * kPhaseWindowSeconds;
        if (++phaseSamples_ == 1) {
            phaseWindowDue_ = true;
            nextPhaseWindowFrame_ = boundary + windowFrames;
        } else if (boundary >= nextPhaseWindowFrame_) {
            phaseWindowDue_ = true;
            nextPhaseWindowFrame_ +=
                ((boundary - nextPhaseWindowFrame_) / windowFrames + 1) * windowFrames;
        }
    }

    void ClosePhaseWindow() noexcept {
        ++phaseWindowsClosed_;
        const auto absPhase = rxClock_.AbsolutePhaseSnapshot();
        if (absPhase.valid) {
            FormRelPhase(absPhase.medianTicks);
        }
        rxClock_.BeginWindow();
    }

    // `rel`: the transmit presentation residual against the device's absolute
    // receive phase, conditioned and handed to the transmit regulator.
    void FormRelPhase(int64_t rxAbsTicks) noexcept {
        auto* control = clockControl_;
        if (control == nullptr ||
            !::ASFW::Audio::Runtime::MotuRelPhaseGeometryHolds(sampleRateHz_)) {
            return;
        }
        ::ASFW::Audio::Runtime::MotuPhaseTraceCadenceSample period{};
        uint64_t traceUpdates = 0;
        if (!control->motuPhaseTrace.ReadLatest(period, traceUpdates)) {
            return;
        }
        const uint64_t generation = control->generation.load(std::memory_order_acquire);
        const auto residual = ::ASFW::Audio::Runtime::ComputeMotuTxPhaseResidualCadenceMean(period);
        if (period.streamGeneration != generation || !residual.valid) {
            return;
        }
        const int64_t rel = ::ASFW::Encoding::Motu::TickDifference(residual.residualTicks, rxAbsTicks);
        ++relPhaseFormed_;
        lastRelTicks_ = rel;
        const auto conditioned = relPhase_.Accept(rel);
        if (conditioned.valid) {
            const ::ASFW::Audio::Runtime::MotuRelPhaseSampleRecord sample{
                .streamGeneration = generation,
                .ticks = conditioned.ticks,
                .centerTicks = conditioned.centerTicks,
            };
            if (control->motuRelPhase.Publish(sample)) {
                ++relPhasePublished_;
            } else {
                ++relPhaseRejected_;
            }
        }
    }

    ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource_{nullptr};
    MotuRxDiagnosticCapture* diagnosticCapture_{nullptr};
    uint32_t sampleRateHz_{0};
    uint64_t diagnosticGuid_{0};
    uint32_t diagnosticStride_{0};
    bool timingEstablished_{false};

    // The receive poll and the telemetry drain are serialized on the same
    // dispatch queue, so these are plain state.
    ::ASFW::Audio::Runtime::MotuRxSphRateMeter rxClock_{};
    ::ASFW::Audio::Runtime::AudioTransportControlBlock* clockControl_{nullptr};
    // Compared only, never dereferenced: identifies the stream the meter's
    // totals belong to.
    const ::ASFW::Audio::Runtime::AudioTransportControlBlock* clockGenerationControl_{nullptr};
    uint64_t clockGeneration_{0};

    // Absolute-phase sampling since Reset(), and the window it feeds.
    uint64_t framesSinceReset_{0};
    uint64_t phaseSamples_{0};
    uint64_t nextPhaseWindowFrame_{0};
    bool phaseWindowDue_{false};
    uint64_t phaseWindowsClosed_{0};

    // `rel`: one conditioner per stream generation, beside the meter that
    // feeds it, so the same boundary resets both.
    ::ASFW::Encoding::Motu::MotuRelPhaseConditioner relPhase_{};
    uint64_t relPhaseFormed_{0};
    int64_t lastRelTicks_{0};
    uint64_t relPhasePublished_{0};
    uint64_t relPhaseRejected_{0};
};

// The producer invokes this before every packet, including NO-DATA. Cadence
// still follows RX; only presentation timestamps use the synthesized clock.
struct MotuSphServoRuntimeInput final {
    bool valid{false};
    uint64_t streamGeneration{0}, expectedStreamGeneration{0}, bridgeUpdates{0}, rxFrames{0};
    int64_t rxTicks{0};
    bool haveRelPhase{false};
    int64_t relPhaseTicks{0}, relPhaseCenterTicks{0};
};
struct MotuSphServoApplyResult final {
    bool observationApplied{false}, stepApplied{false}, muteActivated{false};
    bool hardResyncStarted{false}, phaseRepairApplied{false}, unmuteActivated{false};
    bool acquisitionRestarted{false}, absolutePhaseAcquired{false}, outputMuted{false};
    ::ASFW::Encoding::Motu::SphSynthesizer::State decision{};
};
class MotuTxTimingStamper final : public ::ASFW::Audio::ITxDeviceTimingStamper {
public:
    [[nodiscard]] bool Configure(uint32_t dbs, uint32_t rate, uint32_t lead,
                                 std::optional<int32_t> setpoint) noexcept {
        dbs_ = dbs;
        lead_ = lead;
        setpoint_ = setpoint;
        const bool configured = synthesizer_.Configure(rate, setpoint);
        PublishReady(false);
        return configured;
    }
    void ResetForStart() noexcept { synthesizer_.Reset(); lastFirstSph_ = 0; PublishReady(false); }
    void BindReadiness(std::atomic<uint32_t>* readiness) noexcept { readiness_ = readiness; }
    ::ASFW::Audio::TxTimingStampResult StampPacket(
        const Protocols::Audio::AMDTP::TxPacketSlotView& slot,
        const Protocols::Audio::AMDTP::PreparedTxPacket& packet,
        const Protocols::Audio::AMDTP::AmdtpTimingState& timing) noexcept override {
        using Result = ::ASFW::Audio::TxTimingStampResult;
        const uint32_t blocks = packet.framesInPacket;
        if (!slot.bytes || !dbs_ || !blocks) return Result::kNotApplicable;
        if (packet.byteCount > slot.capacityBytes) return Result::kTimingUnavailable;
        auto payload = std::span<uint8_t>(slot.bytes, packet.byteCount);
        if (payload.size() < 8ULL + uint64_t{dbs_} * 4 * blocks) return Result::kTimingUnavailable;
        if (timing.transmitCycleValid) (void)synthesizer_.SeedOnce(timing.transmitTicks + lead_);
        if (!IsSeeded()) {
            (void)::ASFW::Encoding::Motu::WritePacketSphZero(payload, dbs_, blocks, 8);
            PublishReady(false);
            return Result::kTimingUnavailable;
        }
        for (uint32_t block = 0; block < blocks; ++block) {
            const uint32_t sph = synthesizer_.NextSph();
            if (!block) lastFirstSph_ = sph;
            ::ASFW::Encoding::Motu::WriteSph(payload.subspan(8ULL + uint64_t{block} * dbs_ * 4, 4), sph);
        }
        PublishReady(synthesizer_.Current().locked);
        return Result::kOk;
    }
    [[nodiscard]] bool IsSytUnaware() const noexcept override { return true; }
    [[nodiscard]] MotuSphServoApplyResult ApplyServoInput(const MotuSphServoRuntimeInput& input) noexcept {
        MotuSphServoApplyResult result{};
        const auto before = synthesizer_.Current();
        if (input.valid && input.streamGeneration == input.expectedStreamGeneration) {
            result.observationApplied = synthesizer_.Observe(input.streamGeneration, input.bridgeUpdates,
                input.rxFrames, input.rxTicks, input.haveRelPhase ? std::optional{input.relPhaseTicks} : std::nullopt,
                input.relPhaseCenterTicks);
        }
        result.decision = synthesizer_.Current();
        const auto& state = result.decision;
        result.stepApplied = result.observationApplied;
        result.outputMuted = state.outputMuted;
        result.muteActivated = !before.outputMuted && state.outputMuted;
        result.unmuteActivated = before.outputMuted && !state.outputMuted;
        result.acquisitionRestarted = before.streamGeneration && state.streamGeneration != before.streamGeneration;
        result.absolutePhaseAcquired = result.observationApplied &&
            state.referenceCause == ::ASFW::Encoding::Motu::MotuSphReferenceCause::kAbsolutePhaseSeed;
        result.phaseRepairApplied = result.hardResyncStarted = result.observationApplied && state.phaseRepairApplied;
        PublishReady(state.seeded && state.locked && state.streamGeneration == input.expectedStreamGeneration);
        return result;
    }
    // One state snapshot, rather than a second manually mirrored telemetry model.
    [[nodiscard]] auto ServoTelemetrySnapshot() const noexcept { return synthesizer_.Current(); }
    [[nodiscard]] bool IsOutputMuted() const noexcept { return synthesizer_.Current().outputMuted; }
    [[nodiscard]] bool IsConfigured() const noexcept { return synthesizer_.Current().configured; }
    [[nodiscard]] bool IsSeeded() const noexcept { return synthesizer_.Current().seeded; }
    [[nodiscard]] int64_t StepQ32() const noexcept { return synthesizer_.Current().stepQ32; }
    [[nodiscard]] int64_t AppliedCorrectionQ32() const noexcept { return synthesizer_.Current().txCorrectionQ32; }
    [[nodiscard]] uint64_t DecisionIntervalFrames() const noexcept { return synthesizer_.DecisionIntervalFrames(); }
    [[nodiscard]] uint32_t PresentationLeadTicks() const noexcept { return lead_; }
    [[nodiscard]] bool HoldsAbsolutePhase() const noexcept { return setpoint_.has_value(); }
    [[nodiscard]] int32_t AbsolutePhaseSetpointTicks() const noexcept { return setpoint_.value_or(0); }
    [[nodiscard]] uint32_t LastFirstSph() const noexcept { return lastFirstSph_; }
private:
    void PublishReady(bool ready) noexcept {
        if (readiness_) readiness_->store(ready && IsSeeded() ? 1U : 0U, std::memory_order_release);
    }
    ::ASFW::Encoding::Motu::SphSynthesizer synthesizer_{};
    std::atomic<uint32_t>* readiness_{nullptr};
    uint32_t dbs_{0}, lead_{0}, lastFirstSph_{0};
    std::optional<int32_t> setpoint_{};
};
} // namespace ASFW::Audio::Wire
