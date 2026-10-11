// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuTxPhaseTracePublisher.hpp - the transmit half of the MOTU absolute
// presentation-phase measurement.
//
// Bound per start as the slot provider's release observer, it reads each
// released DATA packet's first SPH -- what the timing stamper wrote -- and
// publishes one cadence period at a time over the control block's
// motuPhaseTrace bridge, with the newest completed (OUTPUT_LAST) packet as the
// bus-time reference and the model's presentation lead. The RX timing observer
// turns that into the TX presentation residual and `rel` (MotuPhaseTrace.hpp).
//
// It sees a packet after the SPH is written and before the slot is committed
// to transport; host PCM is filled later and is not there yet, so nothing here
// looks past the first block's SPH.

#pragma once

#include "AudioTransportControlBlock.hpp"
#include "MotuPhaseTrace.hpp"
#include "../../Ports/IAmdtpTxSlotProvider.hpp"
#include "../../Wire/MOTU/MotuBlockCodec.hpp"
#include "../../Wire/MOTU/MotuSph.hpp"
#include "../../../Isoch/Core/IsochTxQueue.hpp"

#include <atomic>
#include <cstdint>
#include <span>

namespace ASFW::Audio::Runtime {

// Threading: OnRelease() runs on the TX preparation queue. Bind() and Unbind()
// run while the stream is armed or torn down, when no packet is released.
class MotuTxPhaseTracePublisher final
    : public ::ASFW::Protocols::Audio::AMDTP::ITxPacketReleaseObserver {
public:
    void Bind(AudioTransportControlBlock* control,
              const ::ASFW::Isoch::IsochTxQueueControl* queue,
              uint32_t presentationLeadTicks) noexcept {
        control_ = control;
        queue_ = queue;
        presentationLeadTicks_ = presentationLeadTicks;
        // The window may still hold the previous stream's packets, whose SPH
        // belongs to another phase.
        window_ = {};
        published_ = 0;
        rejected_ = 0;
    }

    void Unbind() noexcept {
        control_ = nullptr;
        queue_ = nullptr;
        window_ = {};
    }

    [[nodiscard]] bool IsBound() const noexcept {
        return control_ != nullptr && queue_ != nullptr;
    }

    void OnRelease(const ::ASFW::Protocols::Audio::AMDTP::PreparedTxPacket& packet,
                   std::span<const uint8_t> packetBytes) noexcept override {
        if (!IsBound()) {
            return;
        }
        uint32_t firstSph = 0;
        if (!ReadFirstSph(packet, packetBytes, firstSph)) {
            return;
        }
        // Every DATA packet slides into the window; the window is published
        // once per cadence period, so a reader always gets a whole period.
        PushMotuPhaseTraceCadencePacket(window_, packet.packetIndex, firstSph);
        if (!MotuPhaseTraceSamplesThisPacket(packet.firstAudioFrame, packet.framesInPacket,
                                             kMotuPhaseTraceDataPacketStride)) {
            return;
        }

        // One OUTPUT_LAST reference for the whole period: the projection
        // subtracts the cycles it adds, so which completion it starts from
        // changes no residual, and this stays one stamp read per period.
        uint64_t outputLastPacketIndex = 0;
        uint32_t outputLastCycleTimer = 0;
        bool hasOutputLast = false;
        const uint64_t stamps = queue_->completionStampCount.load(std::memory_order_acquire);
        if (stamps != 0) {
            hasOutputLast =
                queue_->ReadCompletionStamp(stamps - 1, outputLastPacketIndex, outputLastCycleTimer);
        }
        window_.outputLastPacketIndex = hasOutputLast ? outputLastPacketIndex : 0;
        window_.outputLastCycleTimer = hasOutputLast ? outputLastCycleTimer : 0;
        window_.hasOutputLast = hasOutputLast;
        window_.presentationLeadTicks = presentationLeadTicks_;
        // The control block's live generation: the bridge refuses any other,
        // so a period released across a restart cannot reach the new stream.
        window_.streamGeneration = control_->generation.load(std::memory_order_acquire);
        if (control_->motuPhaseTrace.Publish(window_)) {
            ++published_;
        } else {
            ++rejected_;
        }
    }

    // The SPH of the packet's first block as released: the quadlet after the
    // 8-byte CIP header, in every MOTU protocol version. Only a DATA packet
    // carries one, and only a well-formed one is a presentation time.
    [[nodiscard]] static bool ReadFirstSph(
        const ::ASFW::Protocols::Audio::AMDTP::PreparedTxPacket& packet,
        std::span<const uint8_t> packetBytes, uint32_t& outSph) noexcept {
        constexpr uint32_t kCipHeaderBytes = 8;
        constexpr uint32_t kSphBytes = 4;
        if (!packet.isData || packet.framesInPacket == 0 ||
            packet.byteCount < kCipHeaderBytes + kSphBytes ||
            packetBytes.size() < kCipHeaderBytes + kSphBytes) {
            return false;
        }
        const uint32_t sph =
            ::ASFW::Encoding::Motu::ReadSph(packetBytes.subspan(kCipHeaderBytes, kSphBytes));
        if (!::ASFW::Encoding::Motu::IsWellFormedSph(sph)) {
            return false;
        }
        outSph = sph;
        return true;
    }

    // Periods the bridge took, and periods it refused (another generation),
    // since the last Bind(). TX preparation queue only.
    [[nodiscard]] uint64_t Published() const noexcept { return published_; }
    [[nodiscard]] uint64_t Rejected() const noexcept { return rejected_; }

private:
    AudioTransportControlBlock* control_{nullptr};
    const ::ASFW::Isoch::IsochTxQueueControl* queue_{nullptr};
    uint32_t presentationLeadTicks_{0};
    MotuPhaseTraceCadenceSample window_{};
    uint64_t published_{0};
    uint64_t rejected_{0};
};

} // namespace ASFW::Audio::Runtime
