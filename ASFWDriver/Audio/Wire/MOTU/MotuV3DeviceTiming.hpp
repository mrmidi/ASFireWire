// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuV3DeviceTiming.hpp - capture-side timing for MOTU protocol-v3 (828 Mk3).
//
// V2 establishes capture timing by caching each block's SPH for TX replay
// (MotuRxTimingObserver). V3 does not replay: its transmit timing is the PI SPH
// servo, and the HAL clock comes from the Receive hardware timeline, which
// anchors on packet arrival like every other non-M-Audio device. The observer
// owes the consumer two things.
//
// The gate: capture timing is established once a decoded data packet has been
// seen since the last Reset(). That is what the V3 decoder used to assume
// unconditionally, and it is what releases the deferred transmit start --
// waiting for an SYT cadence instead would never succeed, since V3 sends SYT
// NO_INFO.
//
// The servo's RX inputs, observed on the receive path:
//  - the SPH rate meter, whose cumulative frames/ticks travel to TX over the
//    `motuRxSphClock` bridge on every data packet;
//  - the absolute phase (SPH minus receive cycle), sampled once per HAL
//    zero-timestamp period by this observer's own frame count. It used to be
//    sampled from the ZTS record of the packet that published the anchor; the
//    Receive timeline owns that anchor now, so the cadence is the same but the
//    packet within the period may differ;
//  - `[RxPhaseRel]`, conditioned and published over `motuRelPhase`.
// The packet path only measures and records; formatting, the rel bridge and
// the stall edge run in DrainTelemetry(), off the hot path, on the watchdog
// cadence the consumer's telemetry drain always had.
//
// Reset boundaries. The receive consumer calls Reset() on activation,
// on quiesce and on every replay discontinuity. Reset() re-anchors the meter
// rather than clearing it: an internal IR recovery stays in the same audio
// stream generation, and the TX servo needs the cumulative clock to stay
// monotonic across it. Only a new control generation (StartIO) clears the
// meter, and OnBatchBegin() is where that boundary is observed.

#pragma once

#include "../../Ports/IWirePayloadCodec.hpp"
#include "../../DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "../../DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../../DriverKit/Runtime/MotuRxSphRateMeter.hpp"
#include "../../DriverKit/Runtime/MotuServoStallDetector.hpp"
#include "../../Protocols/MOTU/MotuRelPhaseConditioner.hpp"
#include "../../../Common/TimingUtils.hpp"
#include "../../../Isoch/Receive/ZtsTelemetry.hpp"
#include "../../../Shared/Isoch/AudioHalBufferProfiles.hpp"

#include <atomic>
#include <cstdint>
#include <span>

namespace ASFW::Audio::Wire {

class MotuV3RxTimingObserver final : public IRxDeviceTimingObserver {
public:
    // The receive payload starts with the 8-byte OHCI receive prefix and the
    // 8-byte V3 header; the first block's SPH quadlet follows (big-endian).
    static constexpr uint32_t kFirstSphByteOffset = 16;

    MotuV3RxTimingObserver() noexcept = default;

    explicit MotuV3RxTimingObserver(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept
        : bindingSource_(bindingSource) {}

    void OnBatchBegin(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept override {
        if (bindingSource != nullptr) {
            bindingSource_ = bindingSource;
        }
        if (bindingSource_ == nullptr) {
            return;
        }
        ::ASFW::Audio::Runtime::DirectAudioBindingSnapshot snapshot{};
        if (!bindingSource_->CopyDirectAudioBinding(snapshot) || !snapshot.valid ||
            !snapshot.HasInput() || snapshot.control == nullptr) {
            control_ = nullptr;
            return;
        }
        // StartIO may advance the control generation while reusing the same
        // mapped binding, so this boundary is observed independently of
        // binding reconstruction.
        const uint64_t generation =
            snapshot.control->generation.load(std::memory_order_acquire);
        if (snapshot.control != generationControl_ || generation != controlGeneration_) {
            ResetMeasurements();
            generationControl_ = snapshot.control;
            controlGeneration_ = generation;
        }
        control_ = snapshot.control;
        sampleRateHz_ = snapshot.sampleRateHz;
    }

    void ObservePacket(std::span<const uint8_t> payload,
                       uint32_t strideQuadlets,
                       uint32_t framesDecoded,
                       uint16_t cycle) noexcept override {
        (void)strideQuadlets;
        if (framesDecoded == 0) {
            return;
        }
        timingEstablished_ = true;
        if (payload.size() < kFirstSphByteOffset + 4) {
            return;
        }
        const uint8_t* sphBytes = payload.data() + kFirstSphByteOffset;
        const uint32_t sph = (static_cast<uint32_t>(sphBytes[0]) << 24) |
                             (static_cast<uint32_t>(sphBytes[1]) << 16) |
                             (static_cast<uint32_t>(sphBytes[2]) << 8) |
                             static_cast<uint32_t>(sphBytes[3]);
        const uint64_t firstFrame = framesSinceReset_;
        framesSinceReset_ += framesDecoded;

        // Measure the device's own sample period from the slope of its SPH
        // stamps, and hand the cumulative clock to the TX servo.
        rate_.Observe(sph, framesDecoded, sampleRateHz_);
        if (control_ != nullptr) {
            const auto cumulative = rate_.CumulativeSnapshot();
            if (cumulative.valid) {
                (void)control_->motuRxSphClock.Publish({
                    .streamGeneration = control_->generation.load(std::memory_order_acquire),
                    .rxFrames = cumulative.frames,
                    .rxTicks = cumulative.ticks,
                });
            }
        }

        // One absolute-phase sample per HAL zero-timestamp period: the packet
        // that holds the period's first frame. The receive cycle carries offset
        // 0 and MOTU phase lives in a one-second domain, so the cycle alone
        // reconstructs the cycle timer the measurement needs.
        const uint64_t period =
            ::ASFW::IsochTransport::HalBufferProfileForRate(sampleRateHz_)
                .zeroTimestampPeriodFrames;
        if (period == 0) {
            return;
        }
        const uint64_t boundary = ((firstFrame + period - 1) / period) * period;
        if (boundary >= framesSinceReset_) {
            return;
        }
        ::ASFW::Isoch::Rx::ZtsTelemetryRecord record{};
        record.publishCount = ++phaseSamples_;
        record.sampleFrame = boundary;
        record.rxCycleTimer = ::ASFW::Timing::encodeCycleTimer(0, cycle, 0);
        record.framesDecoded = framesDecoded;
        record.motuRxSph = sph;
        record.hasMotuRxSph = true;
        record.kind = static_cast<uint8_t>(phaseSamples_ == 1
            ? ::ASFW::Isoch::Rx::ZtsEventKind::kSeed
            : ::ASFW::Isoch::Rx::ZtsEventKind::kUpdate);
        phaseRecords_.Record(record);
    }

    [[nodiscard]] bool IsTimingEstablished() const noexcept override {
        return timingEstablished_;
    }

    void Reset() noexcept override {
        timingEstablished_ = false;
        rate_.Reanchor();
        stallDetector_.Reset();
        RestartPhaseSampling();
    }

    // Off the hot path, from the consumer's telemetry drain.
    void DrainTelemetry(uint32_t maxRecords) noexcept override;

private:
    void ResetMeasurements() noexcept {
        rate_.Reset();
        relPhase_.Reset();
        stallDetector_.Reset();
        RestartPhaseSampling();
    }

    void RestartPhaseSampling() noexcept {
        framesSinceReset_ = 0;
        phaseSamples_ = 0;
        phaseRecords_.Reset();
        logGate_.Reset();
    }

    void CheckServoStall() noexcept;
    void EmitWindow(uint32_t rate) noexcept;

    ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource_{nullptr};
    // Producer (IR poll) and consumer (watchdog drain) are serialized on the
    // dext's Default queue -- see ZtsTelemetryRing -- so these are plain state
    // shared between the two halves.
    ::ASFW::Audio::Runtime::AudioTransportControlBlock* control_{nullptr};
    ::ASFW::Audio::Runtime::AudioTransportControlBlock* generationControl_{nullptr};
    uint64_t controlGeneration_{0};
    uint32_t sampleRateHz_{0};
    bool timingEstablished_{false};

    ::ASFW::Audio::Runtime::MotuRxSphRateMeter rate_{};
    // Stateful conditioner for `rel`: kept beside the meter that feeds it so
    // one owner resets both on the same boundary.
    ::ASFW::Audio::MOTU::MotuRelPhaseConditioner relPhase_{};
    ::ASFW::Audio::Runtime::MotuServoStallDetector stallDetector_{};

    uint64_t framesSinceReset_{0};
    uint64_t phaseSamples_{0};
    ::ASFW::Isoch::Rx::ZtsTelemetryRing phaseRecords_{};
    ::ASFW::Isoch::Rx::ZtsTelemetryLogGate logGate_{};
};

} // namespace ASFW::Audio::Wire
