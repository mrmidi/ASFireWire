// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Ports/IWirePayloadCodec.hpp"
#include "MotuEventOffsetCache.hpp"
#include "MotuTxTiming.hpp"
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
        uint32_t dbs = 0) noexcept
        : cache_(cache), dbs_(dbs) {}

    void BindCache(::ASFW::Encoding::Motu::MotuEventOffsetCache* cache) noexcept {
        cache_ = cache;
    }

    void Configure(uint32_t dbs) noexcept {
        dbs_ = dbs;
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

        if (cache_ == nullptr || !timing.transmitCycleValid) {
            (void)::ASFW::Encoding::Motu::WritePacketSphZero(
                payload, dbs_, blocks, /*cipHeaderBytes=*/8U);
            return ::ASFW::Audio::TxTimingStampResult::kTimingUnavailable;
        }

        constexpr uint32_t kMaxBlocksPerPacket = 256;
        uint32_t offsets[kMaxBlocksPerPacket]{};
        const uint32_t boundedBlocks = (blocks < kMaxBlocksPerPacket) ? blocks : kMaxBlocksPerPacket;
        if (cache_->Take(std::span<uint32_t>(offsets, boundedBlocks))) {
            (void)::ASFW::Encoding::Motu::WritePacketSph(
                payload, dbs_, boundedBlocks, timing.transmitCycle,
                std::span<const uint32_t>(offsets, boundedBlocks), /*cipHeaderBytes=*/8U);
            return ::ASFW::Audio::TxTimingStampResult::kOk;
        } else {
            (void)::ASFW::Encoding::Motu::WritePacketSphZero(
                payload, dbs_, boundedBlocks, /*cipHeaderBytes=*/8U);
            return ::ASFW::Audio::TxTimingStampResult::kTimingUnavailable;
        }
    }

    [[nodiscard]] bool IsSytUnaware() const noexcept override {
        return true;
    }

private:
    ::ASFW::Encoding::Motu::MotuEventOffsetCache* cache_{nullptr};
    uint32_t dbs_{0};
};

} // namespace ASFW::Audio::Wire
