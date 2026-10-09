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

#include <cstdint>
#include <span>

#include "MotuRxDiagnosticCapture.hpp"

namespace ASFW::Audio::Wire {

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
                diagnosticSampleRate_ = snapshot.sampleRateHz;
            } else {
                activeCache_ = nullptr;
                diagnosticSampleRate_ = 0;
            }
        }
    }

    void ObserveRawPacket(
        uint64_t epoch,
        uint16_t rxTimestamp,
        std::span<const uint8_t> rawPayload) noexcept override {
        if (diagnosticCapture_ != nullptr) {
            constexpr uint32_t kMotuCipPrefixBytes = 16;
            diagnosticCapture_->RecordPacket(epoch, rxTimestamp, rawPayload, diagnosticStride_, kMotuCipPrefixBytes, diagnosticGuid_, diagnosticSampleRate_);
        }
    }

    void ObservePacket(
        std::span<const uint8_t> payload,
        uint32_t strideQuadlets,
        uint32_t framesDecoded,
        uint16_t cycle) noexcept override {
        if (activeCache_ == nullptr) {
            return;
        }
        constexpr uint32_t kMotuCipPrefixBytes = 16;
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
    }

private:
    ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource_{nullptr};
    ::ASFW::Encoding::Motu::MotuEventOffsetCache* explicitCache_{nullptr};
    ::ASFW::Encoding::Motu::MotuEventOffsetCache* activeCache_{nullptr};
    MotuRxDiagnosticCapture* diagnosticCapture_{nullptr};
    uint32_t diagnosticSampleRate_{0};
    uint64_t diagnosticGuid_{0};
    uint32_t diagnosticStride_{0};
    bool timingEstablished_{false};
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
