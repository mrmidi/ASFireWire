// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../../Common/TimingUtils.hpp"
#include "../../DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "../../DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../../DriverKit/Runtime/MotuRxSphRateMeter.hpp"
#include "../../Ports/IWirePayloadCodec.hpp"
#include "MotuModel.hpp"
#include "MotuSphSynthesizer.hpp"
#include "MotuTxTiming.hpp"

#include <atomic>
#include <cstdint>
#include <span>

#include "MotuRxDiagnosticCapture.hpp"

namespace ASFW::Audio::Wire {

// RX-owned raw device-clock observation. No control work depends on a log drain.
class MotuRxTimingObserver final : public IRxDeviceTimingObserver {
  public:
    MotuRxTimingObserver() noexcept = default;
    explicit MotuRxTimingObserver(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* source) noexcept
        : bindingSource_(source) {}
    void BindSource(::ASFW::Audio::Runtime::IDirectAudioBindingSource* source) noexcept {
        bindingSource_ = source;
    }
    void BindDiagnosticCapture(MotuRxDiagnosticCapture* capture, uint32_t stride = 0,
                               uint64_t guid = 0) noexcept {
        diagnosticCapture_ = capture;
        diagnosticStride_ = stride;
        diagnosticGuid_ = guid;
    }
    void OnBatchBegin(::ASFW::Audio::Runtime::IDirectAudioBindingSource* source) noexcept override {
        if (source)
            bindingSource_ = source;
        ::ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
        if (!bindingSource_ || !bindingSource_->CopyDirectAudioBinding(snapshot) ||
            !snapshot.control) {
            sampleRateHz_ = 0;
            clockControl_ = nullptr;
            return;
        }
        const uint64_t generation = snapshot.control->generation.load(std::memory_order_acquire);
        if (snapshot.control != clockGenerationControl_ || generation != clockGeneration_) {
            timingEstablished_ = false;
            rxClock_.Reset();
            clockGenerationControl_ = snapshot.control;
            clockGeneration_ = generation;
        }
        sampleRateHz_ = snapshot.sampleRateHz;
        clockControl_ = snapshot.control;
    }
    void ObserveRawPacket(uint64_t epoch, uint16_t timestamp,
                          std::span<const uint8_t> payload) noexcept override {
        if (diagnosticCapture_)
            diagnosticCapture_->RecordPacket(epoch, timestamp, payload, diagnosticStride_,
                                             kMotuCipPrefixBytes, diagnosticGuid_, sampleRateHz_);
    }
    void ObservePacket(std::span<const uint8_t> payload, uint32_t stride, uint32_t frames, uint16_t,
                       uint64_t firstAudioFrame) noexcept override {
        if (!clockControl_ || !frames || !stride || payload.size() < kMotuCipPrefixBytes + 4U)
            return;
        const uint32_t sph =
            ::ASFW::Encoding::Motu::ReadSph(payload.subspan(kMotuCipPrefixBytes, 4));
        if (!::ASFW::Encoding::Motu::IsWellFormedSph(sph))
            return;
        rxClock_.Observe(sph, frames, sampleRateHz_);
        timingEstablished_ = true;
        const auto cumulative = rxClock_.CumulativeSnapshot();
        if (cumulative.valid) {
            (void)clockControl_->motuRxSphClock.Publish(
                {.streamGeneration = clockGeneration_,
                 .rxFrames = cumulative.frames,
                 .rxTicks = cumulative.ticks,
                 .firstAudioFrame = firstAudioFrame,
                 .sphTicks = ::ASFW::Encoding::Motu::TickFromSph(sph),
                 .replayEpoch = clockControl_->rxSequenceReplay.Epoch()});
        }
    }
    [[nodiscard]] bool IsTimingEstablished() const noexcept override {
        return timingEstablished_ && clockControl_;
    }
    void Reset() noexcept override {
        timingEstablished_ = false;
        rxClock_.Reanchor();     // Preserve accepted cumulative rate totals within a start.
        clockControl_ = nullptr; // Quiesce drops the mapped view before it can be freed.
    }

  private:
    static constexpr uint32_t kMotuCipPrefixBytes = 16;
    ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource_{nullptr};
    MotuRxDiagnosticCapture* diagnosticCapture_{nullptr};
    uint32_t sampleRateHz_{0}, diagnosticStride_{0};
    uint64_t diagnosticGuid_{0}, clockGeneration_{0};
    bool timingEstablished_{false};
    ::ASFW::Audio::Runtime::MotuRxSphRateMeter rxClock_{};
    ::ASFW::Audio::Runtime::AudioTransportControlBlock* clockControl_{nullptr};
    // Identity only, never dereferenced after quiesce.
    const ::ASFW::Audio::Runtime::AudioTransportControlBlock* clockGenerationControl_{nullptr};
};

// The producer invokes this before every packet, including NO-DATA. Cadence
// still follows RX; only presentation timestamps use the synthesized clock.
struct MotuSphServoRuntimeInput final {
    bool valid{false};
    uint64_t streamGeneration{0}, expectedStreamGeneration{0}, bridgeUpdates{0}, rxFrames{0};
    int64_t rxTicks{0};
    uint64_t rxFirstFrame{0}, txFirstFrame{0};
    uint32_t rxSphTicks{0}, replayEpoch{0}, expectedReplayEpoch{0};
    bool isData{false}, transmitCycleValid{false};
    int64_t transmitTicks{0};
};
struct MotuSphServoApplyResult final {
    bool observationApplied{false}, stepApplied{false}, muteActivated{false};
    bool hardResyncStarted{false}, phaseRepairApplied{false}, unmuteActivated{false};
    bool acquisitionRestarted{false}, outputMuted{false};
    ::ASFW::Encoding::Motu::SphSynthesizer::State decision{};
};
class MotuTxTimingStamper final : public ::ASFW::Audio::ITxDeviceTimingStamper {
  public:
    [[nodiscard]] bool Configure(uint32_t dbs, uint32_t rate, uint32_t lead,
                                 uint32_t offsetFrames = 13) noexcept {
        dbs_ = dbs;
        lead_ = lead;
        const bool configured = synthesizer_.Configure(rate, offsetFrames);
        PublishReady(false);
        return configured;
    }
    void ResetForStart() noexcept {
        synthesizer_.Reset();
        lastFirstSph_ = 0;
        PublishReady(false);
    }
    void BindReadiness(std::atomic<uint32_t>* readiness) noexcept { readiness_ = readiness; }
    ::ASFW::Audio::TxTimingStampResult
    StampPacket(const Protocols::Audio::AMDTP::TxPacketSlotView& slot,
                const Protocols::Audio::AMDTP::PreparedTxPacket& packet,
                const Protocols::Audio::AMDTP::AmdtpTimingState& timing) noexcept override {
        using Result = ::ASFW::Audio::TxTimingStampResult;
        const uint32_t blocks = packet.framesInPacket;
        if (!slot.bytes || !dbs_ || !blocks)
            return Result::kNotApplicable;
        if (packet.byteCount > slot.capacityBytes)
            return Result::kTimingUnavailable;
        auto payload = std::span<uint8_t>(slot.bytes, packet.byteCount);
        if (payload.size() < 8ULL + uint64_t{dbs_} * 4 * blocks)
            return Result::kTimingUnavailable;
        if (timing.transmitCycleValid)
            (void)synthesizer_.SeedOnce(timing.transmitTicks + lead_);
        if (!IsSeeded()) {
            (void)::ASFW::Encoding::Motu::WritePacketSphZero(payload, dbs_, blocks, 8);
            PublishReady(false);
            return Result::kTimingUnavailable;
        }
        for (uint32_t block = 0; block < blocks; ++block) {
            const uint32_t sph = synthesizer_.NextSph();
            if (!block)
                lastFirstSph_ = sph;
            ::ASFW::Encoding::Motu::WriteSph(payload.subspan(8ULL + uint64_t{block} * dbs_ * 4, 4),
                                             sph);
        }
        PublishReady(synthesizer_.Current().locked);
        return Result::kOk;
    }
    [[nodiscard]] bool IsSytUnaware() const noexcept override { return true; }
    [[nodiscard]] MotuSphServoApplyResult
    ApplyServoInput(const MotuSphServoRuntimeInput& input) noexcept {
        MotuSphServoApplyResult result{};
        const auto before = synthesizer_.Current();
        synthesizer_.BeginGeneration(input.expectedStreamGeneration);
        if (input.isData && input.transmitCycleValid)
            (void)synthesizer_.SeedOnce(input.transmitTicks + lead_);
        if (input.isData && input.valid &&
            input.streamGeneration == input.expectedStreamGeneration &&
            input.replayEpoch == input.expectedReplayEpoch) {
            result.observationApplied = synthesizer_.Observe({.generation = input.streamGeneration,
                                                              .updates = input.bridgeUpdates,
                                                              .frames = input.rxFrames,
                                                              .ticks = input.rxTicks,
                                                              .rxFirstFrame = input.rxFirstFrame,
                                                              .txFirstFrame = input.txFirstFrame,
                                                              .sphTicks = input.rxSphTicks,
                                                              .replayEpoch = input.replayEpoch});
        }
        result.decision = synthesizer_.Current();
        const auto& state = result.decision;
        result.stepApplied = result.observationApplied && state.feedbackUpdated;
        result.outputMuted = state.outputMuted;
        result.muteActivated = !before.outputMuted && state.outputMuted;
        result.unmuteActivated = before.outputMuted && !state.outputMuted;
        result.acquisitionRestarted =
            (before.streamGeneration && state.streamGeneration != before.streamGeneration) ||
            (result.observationApplied &&
             state.referenceCause == ::ASFW::Encoding::Motu::MotuSphReferenceCause::kRxRebind);
        result.phaseRepairApplied = result.hardResyncStarted =
            result.observationApplied && state.phaseRepairApplied;
        PublishReady(state.seeded && state.locked &&
                     state.streamGeneration == input.expectedStreamGeneration);
        return result;
    }
    // One state snapshot, rather than a second manually mirrored telemetry model.
    [[nodiscard]] auto ServoTelemetrySnapshot() const noexcept { return synthesizer_.Current(); }
    [[nodiscard]] bool IsOutputMuted() const noexcept { return synthesizer_.Current().outputMuted; }
    [[nodiscard]] bool IsConfigured() const noexcept { return synthesizer_.Current().configured; }
    [[nodiscard]] bool IsSeeded() const noexcept { return synthesizer_.Current().seeded; }
    [[nodiscard]] int64_t StepQ32() const noexcept { return synthesizer_.Current().stepQ32; }
    [[nodiscard]] int64_t AppliedCorrectionQ32() const noexcept {
        return synthesizer_.Current().txCorrectionQ32;
    }
    [[nodiscard]] uint64_t DecisionIntervalFrames() const noexcept {
        return synthesizer_.DecisionIntervalFrames();
    }
    [[nodiscard]] uint32_t PresentationLeadTicks() const noexcept { return lead_; }
    [[nodiscard]] uint32_t PresentationOffsetFrames() const noexcept {
        return synthesizer_.PresentationOffsetFrames();
    }
    [[nodiscard]] uint32_t LastFirstSph() const noexcept { return lastFirstSph_; }

  private:
    void PublishReady(bool ready) noexcept {
        if (readiness_)
            readiness_->store(ready && IsSeeded() ? 1U : 0U, std::memory_order_release);
    }
    ::ASFW::Encoding::Motu::SphSynthesizer synthesizer_{};
    std::atomic<uint32_t>* readiness_{nullptr};
    uint32_t dbs_{0}, lead_{0}, lastFirstSph_{0};
};
} // namespace ASFW::Audio::Wire
