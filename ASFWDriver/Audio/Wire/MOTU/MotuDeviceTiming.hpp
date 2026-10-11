// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Ports/IWirePayloadCodec.hpp"
#include "MotuEventOffsetCache.hpp"
#include "MotuTxTiming.hpp"
#include "MotuModel.hpp"
#include "MotuSphSynthesizer.hpp"
#include "../../DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../../DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "../../DriverKit/Runtime/MotuRxSphRateMeter.hpp"
#include "../../DriverKit/Runtime/MotuServoStallDetector.hpp"
#include "../../../Logging/Logging.hpp"

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
                 "[MotuServoStall] reason=rx-meter-behind-bridge gen=%llu updates=%llu "
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

class MotuTxTimingStamper final : public ::ASFW::Audio::ITxDeviceTimingStamper {
public:
    MotuTxTimingStamper() noexcept = default;

    explicit MotuTxTimingStamper(
        ::ASFW::Encoding::Motu::MotuEventOffsetCache* cache,
        uint32_t dbs = 0,
        ::ASFW::Encoding::Motu::TimingPolicy policy = ::ASFW::Encoding::Motu::TimingPolicy::ReplayObserved) noexcept
        : policy_(policy), cache_(cache), dbs_(dbs) { synthesizer_.Configure(48000); }

    void BindCache(::ASFW::Encoding::Motu::MotuEventOffsetCache* cache,
                   std::atomic<uint32_t>* readiness = nullptr) noexcept {
        const uint32_t epoch = cache ? cache->Epoch() : 0;
        const bool changed = cache != cache_ || epoch != cacheEpoch_;
        if (changed) synthesizer_.Reset();
        cacheEpoch_ = epoch;
        cache_ = cache;
        readiness_ = readiness;
        if (changed) PublishReady(false);
    }

    void Configure(uint32_t dbs, uint32_t rateHz = 48000,
                   ::ASFW::Encoding::Motu::TimingPolicy policy = ::ASFW::Encoding::Motu::TimingPolicy::ReplayObserved) noexcept {
        dbs_ = dbs;
        policy_ = policy;
        synthesizer_.Configure(rateHz);
        PublishReady(false);
    }

    ::ASFW::Audio::TxTimingStampResult StampPacket(
        const Protocols::Audio::AMDTP::TxPacketSlotView& slot,
        const Protocols::Audio::AMDTP::PreparedTxPacket& packet,
        const Protocols::Audio::AMDTP::AmdtpTimingState& timing) noexcept override {
        const uint32_t blocks = packet.framesInPacket;
        if (slot.bytes == nullptr || dbs_ == 0 || blocks == 0) {
            return ::ASFW::Audio::TxTimingStampResult::kNotApplicable;
        }

        auto payload = std::span<uint8_t>(slot.bytes, packet.byteCount);

        if (cache_ && cacheEpoch_ != cache_->Epoch()) {
            cacheEpoch_ = cache_->Epoch();
            synthesizer_.Reset();
            PublishReady(false);
        }
        if (cache_ == nullptr || !timing.transmitCycleValid) {
            PublishReady(false);
            (void)::ASFW::Encoding::Motu::WritePacketSphZero(
                payload, dbs_, blocks, /*cipHeaderBytes=*/8U);
            return ::ASFW::Audio::TxTimingStampResult::kTimingUnavailable;
        }

        constexpr uint32_t kMaxBlocksPerPacket = 256;
        uint32_t offsets[kMaxBlocksPerPacket]{};
        const uint32_t boundedBlocks = (blocks < kMaxBlocksPerPacket) ? blocks : kMaxBlocksPerPacket;
        if (cache_->Take(std::span<uint32_t>(offsets, boundedBlocks))) {
            const auto firstTick = (::ASFW::Encoding::Motu::BaseTickForCycle(timing.transmitCycle) + offsets[0]) %
                ::ASFW::Encoding::Motu::kTicksPerSecond;
            if (boundedBlocks != blocks || payload.size() < 8ULL + uint64_t{dbs_} * 4 * blocks ||
                (policy_ == ::ASFW::Encoding::Motu::TimingPolicy::SynthesizedExperimental &&
                 !synthesizer_.Observe(firstTick))) {
                PublishReady(false);
                (void)::ASFW::Encoding::Motu::WritePacketSphZero(payload, dbs_, blocks);
                return ::ASFW::Audio::TxTimingStampResult::kTimingUnavailable;
            }
            if (policy_ == ::ASFW::Encoding::Motu::TimingPolicy::ReplayObserved) {
                (void)::ASFW::Encoding::Motu::WritePacketSph(payload, dbs_, blocks,
                    timing.transmitCycle, std::span<const uint32_t>(offsets, blocks));
                PublishReady(true);
            } else {
                for (uint32_t block = 0; block < blocks; ++block)
                    ::ASFW::Encoding::Motu::WriteSph(payload.subspan(8U + block * dbs_ * 4U, 4), synthesizer_.NextSph());
                PublishReady(synthesizer_.Current().locked);
            }
            return ::ASFW::Audio::TxTimingStampResult::kOk;
        } else {
            PublishReady(false);
            (void)::ASFW::Encoding::Motu::WritePacketSphZero(
                payload, dbs_, boundedBlocks, /*cipHeaderBytes=*/8U);
            return ::ASFW::Audio::TxTimingStampResult::kTimingUnavailable;
        }
    }

    [[nodiscard]] bool IsSytUnaware() const noexcept override {
        return true;
    }

private:
    void PublishReady(bool ready) noexcept {
        if (readiness_) readiness_->store(ready ? 1U : 0U, std::memory_order_release);
    }
    std::atomic<uint32_t>* readiness_{nullptr};
    ::ASFW::Encoding::Motu::TimingPolicy policy_{::ASFW::Encoding::Motu::TimingPolicy::ReplayObserved};
    ::ASFW::Encoding::Motu::MotuEventOffsetCache* cache_{nullptr};
    uint32_t dbs_{0};
    uint32_t cacheEpoch_{0};
    ::ASFW::Encoding::Motu::SphSynthesizer synthesizer_;
};

} // namespace ASFW::Audio::Wire
