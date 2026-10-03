#pragma once

#include "AudioTransportControlBlock.hpp"
#include "IsochOracleCapture.hpp"
#include "MotuPhaseTrace.hpp"
#include "../../Wire/AMDTP/AmdtpTypes.hpp"
#include "../../../Isoch/Core/IsochTxQueue.hpp"

#include <cstdint>

namespace ASFW::Audio::Runtime {

/// MOTU protocol-v3 instruments that read a TX packet at its release, from the
/// slot provider's PublishSlot. Bound for a V3 stream only, so
/// no other family pays for them on its packet path.
///
/// Both read only what the packet already carries when it is released: the CIP
/// header and the SPH that MotuV3TxTimingStamper wrote. PCM is not there yet
/// (the audio fill comes later), which is why the PCM scan that
/// used to sit here was retired (`[TxWire]`, TX_OWNERSHIP.md T8) and why nothing below
/// looks past the first block's SPH.
///
/// Owned and called by the TX preparation queue only.
class MotuV3TxPublishInstruments final {
public:
    void Bind(AudioTransportControlBlock* control,
              const ::ASFW::Isoch::IsochTxQueueControl* queue) noexcept {
        control_ = control;
        queue_ = queue;
        // The window holds packets from the previous stream until it is
        // cleared, and their SPH belongs to a different phase domain.
        cadence_ = {};
    }

    void Unbind() noexcept {
        control_ = nullptr;
        queue_ = nullptr;
        cadence_ = {};
    }

    [[nodiscard]] bool IsBound() const noexcept {
        return control_ != nullptr && queue_ != nullptr;
    }

    void OnPublish(const ::ASFW::Protocols::Audio::AMDTP::PreparedTxPacket& packet,
                   const uint8_t* slotBytes) noexcept {
        if (!IsBound()) {
            return;
        }
        uint32_t firstSph = 0;
        const bool hasSph = ReadFirstSph(packet, slotBytes, firstSph);
        if (hasSph) {
            PublishPhaseTrace(packet, firstSph);
        }
        RecordOracleCapture(packet, firstSph, hasSph);
    }

    /// SPH of the packet's first block, as released. A V3 DATA packet carries
    /// one SPH per block; the stamper writes them all, so a DATA packet always
    /// has the first one (MotuV3Wire: CIP header, then SPH, then PCM chunks).
    [[nodiscard]] static bool ReadFirstSph(
        const ::ASFW::Protocols::Audio::AMDTP::PreparedTxPacket& packet,
        const uint8_t* slotBytes, uint32_t& outSph) noexcept {
        constexpr uint32_t kCipHeaderBytes = 8;
        constexpr uint32_t kSphBytes = 4;
        if (slotBytes == nullptr || !packet.isData || packet.framesInPacket == 0 ||
            packet.byteCount < kCipHeaderBytes + kSphBytes) {
            return false;
        }
        const uint8_t* sph = slotBytes + kCipHeaderBytes;
        outSph = (static_cast<uint32_t>(sph[0]) << 24) |
                 (static_cast<uint32_t>(sph[1]) << 16) |
                 (static_cast<uint32_t>(sph[2]) << 8) |
                 static_cast<uint32_t>(sph[3]);
        return true;
    }

private:
    // Keep the last three DATA packets, publish the triple. The
    // publication RATE is once per cadence period, on the frame-cursor gate,
    // and a reader gets a whole period, so it can take the mean -- the only
    // statistic that does not depend on which cadence bucket the cursor landed
    // on. Any three CONSECUTIVE DATA packets carry one of each bucket, so this
    // rolling window needs no period-boundary detection. Publishing every DATA
    // packet is what put the 1024-tick lattice into [RxPhaseRel];
    // MotuPhaseTrace.hpp carries the derivation.
    void PublishPhaseTrace(const ::ASFW::Protocols::Audio::AMDTP::PreparedTxPacket& packet,
                           uint32_t firstSph) noexcept {
        PushMotuPhaseTraceCadencePacket(cadence_, packet.packetIndex, firstSph);
        if (!MotuPhaseTraceSamplesThisPacket(packet.firstAudioFrame, packet.framesInPacket,
                                             kMotuPhaseTraceDataPacketStride48k)) {
            return;
        }
        uint64_t outputLastPacketIndex = 0;
        uint32_t outputLastCycleTimer = 0;
        bool hasOutputLast = false;
        const uint64_t completionCount =
            queue_->completionStampCount.load(std::memory_order_acquire);
        if (completionCount != 0) {
            hasOutputLast = queue_->ReadCompletionStamp(
                completionCount - 1, outputLastPacketIndex, outputLastCycleTimer);
        }
        cadence_.outputLastPacketIndex = outputLastPacketIndex;
        cadence_.outputLastCycleTimer = outputLastCycleTimer;
        cadence_.hasOutputLast = hasOutputLast;
        control_->motuPhaseTrace.Publish(cadence_);
    }

    // Bounded start-window capture for the offline comparison against the
    // passive bus capture of the official driver. Two halves, because a TX packet's own OHCI cycle
    // only exists once hardware has completed it: the wire facts are recorded
    // at release, and the completion cycles are drained and attributed
    // afterwards.
    void RecordOracleCapture(const ::ASFW::Protocols::Audio::AMDTP::PreparedTxPacket& packet,
                             uint32_t firstSph, bool hasSph) noexcept {
        auto& capture = control_->isochOracleCapture;
        capture.RecordTxPrepared(packet.packetIndex, packet.byteCount, packet.isData,
                                 packet.dbc, firstSph, hasSph);
        if (capture.TxCompletionDrainFinished()) {
            return;
        }

        // The transport's stamp ring holds only the most recent
        // kIsochTxCompletionStampSlots completions. Draining here -- the same
        // cadence at which packets are prepared -- keeps up in steady state;
        // anything the ring dropped first is counted rather than inferred.
        const uint64_t available =
            queue_->completionStampCount.load(std::memory_order_acquire);
        uint64_t cursor = capture.TxCompletionCursor();
        for (; cursor < available; ++cursor) {
            uint64_t completedPacketIndex = 0;
            uint32_t completionCycleTimer = 0;
            if (!queue_->ReadCompletionStamp(cursor, completedPacketIndex,
                                             completionCycleTimer)) {
                capture.NoteLostTxCompletion();
                continue;
            }
            capture.BackfillTxCompletionCycle(completedPacketIndex, completionCycleTimer);
            capture.NoteTxCompletionDrained(completedPacketIndex);
        }
        capture.SetTxCompletionCursor(cursor);
    }

    AudioTransportControlBlock* control_{nullptr};
    const ::ASFW::Isoch::IsochTxQueueControl* queue_{nullptr};
    MotuPhaseTraceCadenceSample cadence_{};
};

} // namespace ASFW::Audio::Runtime
