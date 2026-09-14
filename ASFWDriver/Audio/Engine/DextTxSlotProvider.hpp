// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Common/FWCommon.hpp"
#include "../../Isoch/Core/IsochTxQueue.hpp"
#include "../../Logging/Logging.hpp"
#include "../Ports/IAmdtpTxSlotProvider.hpp"
#include "../Shared/AudioTimingGeometry.hpp"
#include "../Shared/TxCycleAnchor.hpp"
#include "../DriverKit/Runtime/AudioTransportControlBlock.hpp"

#include <cstdint>

namespace ASFW::Audio {

class DextTxExecutionTimeline final {
public:
    const ASFW::Isoch::IsochTxQueueControl* queueControl{nullptr};

    [[nodiscard]] bool AnchorForPacket(uint64_t packetIndex,
                                       int64_t& outTicks) const noexcept {
        if (!queueControl) {
            return false;
        }

        const uint64_t count =
            queueControl->completionStampCount.load(std::memory_order_acquire);
        if (count == 0) {
            return false;
        }

        uint64_t completedPacketIndex = 0;
        uint32_t timestamp = 0;
        if (!queueControl->ReadCompletionStamp(
                count - 1, completedPacketIndex, timestamp) ||
            packetIndex < completedPacketIndex) {
            return false;
        }

        // Linux consumes OHCI's 16-bit OUTPUT_LAST status timestamp at
        // firewire/ohci.c:3055. That stamp carries only sec[2:0], so it is
        // lifted here against the full CYCLE_TIMER read the same refill pass
        // published, yielding a transmit time in the same 128-second bus
        // domain the hardware timeline's RX observations use.
        ASFW::Isoch::IsochTxClockPairSample pair{};
        if (!queueControl->clockPair.TryRead(pair)) {
            return false;
        }

        return ASFW::Audio::Shared::TransmitPacketBusTicks(
            timestamp, pair.cycleTimer32,
            packetIndex - completedPacketIndex, outTicks);
    }
};

/// Leading bytes of an AMDTP payload that never differ between images of the
/// same packet: the CIP header.
inline constexpr uint16_t kAmdtpCipHeaderBytes = 8;

/// Inspect one transmit packet in this many. Power of two so the test is a
/// mask. At 8000 packets/s this samples ~125 Hz, which is far finer than the
/// timescale on which programme material goes quiet, while keeping the scan out
/// of 63 of every 64 trips through the transmit hot path.
inline constexpr uint32_t kWirePayloadInspectStride = 64;

class DextTxSlotProvider final : public ASFW::Protocols::Audio::AMDTP::IAmdtpTxSlotProvider {
public:
    uint8_t* payloadBase{nullptr};
    ASFW::Isoch::IsochTxPacketMeta* metadataRing{nullptr};
    ASFW::Isoch::IsochTxQueueControl* queueControl{nullptr};
    ASFW::Audio::Runtime::AudioTransportControlBlock* audioControl{nullptr};
    uint32_t numSlots{0};
    uint32_t slotStrideBytes{0};

    bool AcquireWritableSlot(
        uint32_t packetIndex,
        ASFW::Protocols::Audio::AMDTP::TxPacketSlotView& outSlot)
        noexcept override {
        if (!payloadBase || !queueControl || numSlots == 0 ||
            slotStrideBytes == 0) {
            return false;
        }
        const uint64_t committedEnd =
            queueControl->committedEnd.load(std::memory_order_acquire);
        const uint64_t completionCursor =
            queueControl->completionCursor.load(std::memory_order_acquire);
        if (!ASFW::Isoch::CanAcquireTxProducerSlot(
                packetIndex, committedEnd, completionCursor, numSlots)) {
            ASFW_LOG_ERROR(
                DirectAudio,
                "[TxOwnership] reject acquire packet=%u committed=%llu completion=%llu slots=%u",
                packetIndex,
                committedEnd,
                completionCursor,
                numSlots);
            return false;
        }
        const uint32_t slotIdx = packetIndex % numSlots;
        outSlot.packetIndex = packetIndex;
        outSlot.bytes = payloadBase +
            ASFW::Isoch::TxPayloadImageOffset(slotIdx, 0, slotStrideBytes);
        outSlot.capacityBytes = slotStrideBytes;
        return true;
    }

    [[nodiscard]] uint64_t FinalizedEnd() const noexcept override {
        if (!queueControl) return 0;
        return queueControl->finalizedEnd.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool AcquireLatePayloadSlot(
        uint32_t packetIndex,
        ASFW::Protocols::Audio::AMDTP::TxPacketSlotView& outSlot)
        noexcept override {
        if (!payloadBase || !queueControl || numSlots == 0 ||
            slotStrideBytes == 0) {
            if (queueControl) {
                const uint64_t token = queueControl->ActiveCaptureToken();
                queueControl->RecordProducerAcquire(
                    token, packetIndex, 0,
                    ASFW::Isoch::LatePayloadAcquireResult::RejectedInvalidConfig,
                    /*staleSlotSeen=*/false);
            }
            return false;
        }
        // One load, used for every record written below, so a capture that
        // starts or stops mid-call cannot split this decision across two
        // identities. Zero means no capture, and every Record* call below is
        // then a no-op that reads no clock.
        const uint64_t token = queueControl->ActiveCaptureToken();
        const uint64_t expectedGen =
            ASFW::Isoch::ExpectedTxCommitGeneration(packetIndex, numSlots);

        // The packet must already be armed -- a late image only ever replaces
        // bytes for a packet whose geometry transport has accepted.
        const uint64_t committedEnd =
            queueControl->committedEnd.load(std::memory_order_acquire);
        if (packetIndex >= committedEnd) {
            queueControl->RecordProducerAcquire(
                token, packetIndex, expectedGen,
                ASFW::Isoch::LatePayloadAcquireResult::RejectedNotArmed,
                /*staleSlotSeen=*/false);
            return false;
        }
        // ...and its payload choice must not be final. The producer writes only
        // image 1; transport alone changes a live descriptor address, so a
        // stale frontier can waste a fill but cannot tear transmitted bytes.
        if (packetIndex <
            queueControl->finalizedEnd.load(std::memory_order_acquire)) {
            queueControl->RecordProducerAcquire(
                token, packetIndex, expectedGen,
                ASFW::Isoch::LatePayloadAcquireResult::RejectedFinalized,
                /*staleSlotSeen=*/false);
            return false;
        }
        const uint32_t slotIdx = packetIndex % numSlots;
        // Observed, never acted on. Turning this into a rejection would change
        // what the driver does under measurement, and a telemetry patch that
        // alters the behaviour it measures cannot be used to explain it.
        const bool staleSlotSeen =
            metadataRing != nullptr &&
            metadataRing[slotIdx].packetIndex != packetIndex;

        queueControl->RecordProducerAcquire(
            token, packetIndex, expectedGen,
            ASFW::Isoch::LatePayloadAcquireResult::Success, staleSlotSeen);

        outSlot.packetIndex = packetIndex;
        outSlot.bytes = payloadBase +
            ASFW::Isoch::TxPayloadImageOffset(slotIdx, 1, slotStrideBytes);
        outSlot.capacityBytes = slotStrideBytes;
        return true;
    }

    [[nodiscard]] uint64_t CompletionCursor() const noexcept override {
        if (!queueControl) return 0;
        return queueControl->completionCursor.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool CaptureActive() const noexcept override {
        return queueControl != nullptr &&
               queueControl->ActiveCaptureToken() != 0;
    }

    void RecordEncodingCompleted(
        uint32_t packetIndex, uint64_t hostTicks) noexcept override {
        if (!queueControl) return;
        queueControl->RecordProducerEncode(
            queueControl->ActiveCaptureToken(), packetIndex, hostTicks);
    }

    [[nodiscard]] bool PublishLatePayload(uint32_t packetIndex) noexcept override {
        if (!metadataRing || !queueControl || numSlots == 0) return false;
        const uint32_t slotIdx = packetIndex % numSlots;
        auto& meta = metadataRing[slotIdx];
        if (meta.packetIndex != packetIndex) return false;
        const uint64_t expectedGen =
            ASFW::Isoch::ExpectedTxCommitGeneration(packetIndex, numSlots);
        const uint64_t token = queueControl->ActiveCaptureToken();
        const bool capturing = token != 0;
        uint64_t observedArb = 0;
        const uint64_t startTicks = capturing ? mach_absolute_time() : 0;
        const bool won = ASFW::Isoch::OfferLateTxPayload(
            meta, expectedGen, observedArb);
        const uint64_t endTicks = capturing ? mach_absolute_time() : 0;
        if (capturing) {
            const uint8_t observedPhase = static_cast<uint8_t>(
                observedArb &
                ((1ULL << ASFW::Isoch::kTxPayloadArbitrationPhaseBits) - 1));
            queueControl->RecordProducerOffer(token, packetIndex, expectedGen,
                                              startTicks, endTicks, won,
                                              observedPhase);
        }

        // Attribute the outcome before the inspection's own filter hides it.
        // Two relaxed increments on a path that already does several.
        if (won && audioControl != nullptr) {
            audioControl->txWirePayloadTelemetry.lateOffersWon.fetch_add(
                1, std::memory_order_relaxed);
            if (meta.payloadLength <= kAmdtpCipHeaderBytes) {
                audioControl->txWirePayloadTelemetry.lateWonHeaderOnly.fetch_add(
                    1, std::memory_order_relaxed);
            }
        }

        // `won` is the only point at which image 1 is known to be the bytes the
        // wire will carry, so it is the only honest place to measure content.
        //
        // Sampled, not per-packet. A scan on every packet is a scan inside the
        // transmit hot path, which is instrumentation that changes the thing it
        // measures -- the previous attempt at this took `rebound` from 366 to
        // 145,389. One packet in 64 is ~125 Hz, far above the rate at which
        // programme material goes quiet, and `maxAbs24` is a running maximum so
        // sampling can only understate it.
        if (won && audioControl != nullptr && payloadBase != nullptr &&
            slotStrideBytes != 0 && meta.payloadLength > kAmdtpCipHeaderBytes &&
            (packetIndex & (kWirePayloadInspectStride - 1u)) == 0u) {
            const uint8_t* const latePayload = payloadBase +
                ASFW::Isoch::TxPayloadImageOffset(slotIdx, 1, slotStrideBytes);
            const auto observation =
                audioControl->txWirePayloadTelemetry.Observe(
                    packetIndex, latePayload, meta.payloadLength);
            if (observation.firstInfo || observation.dropout) {
                ASFW_LOG_RING_ONLY_RL(
                    DirectAudio,
                    "tx-wire-payload",
                    observation.firstInfo ? 0u : 1000u,
                    ::ASFW::Logging::LogLevel::Warning,
                    "[TxWire] packet=%u first=%d dropout=%d infoQuads=%u "
                    "maxAbs24=%u lastQuad=0x%08x",
                    packetIndex,
                    observation.firstInfo ? 1 : 0,
                    observation.dropout ? 1 : 0,
                    observation.infoQuads,
                    observation.maxAbs24,
                    observation.lastInfoQuad);
            }
        }
        return won;
    }

    [[nodiscard]] bool PublishSlot(
        const ASFW::Protocols::Audio::AMDTP::PreparedTxPacket& packet)
        noexcept override {
        if (!metadataRing || !queueControl || numSlots == 0) {
            return false;
        }
        if (packet.isData && !packet.pcmFinalized) {
            ASFW_LOG_ERROR(
                DirectAudio,
                "[TxPublish] reject unfinalized data packet=%u frame=%llu",
                packet.packetIndex,
                packet.firstAudioFrame);
            return false;
        }
        const uint32_t slotIdx = packet.packetIndex % numSlots;
        auto& meta = metadataRing[slotIdx];

        meta.packetIndex = packet.packetIndex;
        meta.payloadLength = packet.byteCount;
        // The CIP header leads every AMDTP payload and is identical in both
        // images of a packet -- RefillPcm reproduces it from the armed packet's
        // own DBC and SYT rather than re-deriving it. Declaring it as the
        // invariant prefix lets transport address the sample words through a
        // single descriptor field. Transport is told a byte count, not what the
        // bytes are.
        meta.payloadPrefixBytes = packet.byteCount >= kAmdtpCipHeaderBytes
            ? kAmdtpCipHeaderBytes : uint16_t{0};

        // immediateData[0] = isoch packet header: spd=2 (S400) at [18:16],
        // tag=1 (standard CIP) at [15:14], tcode=0xA (isoch data block
        // transmit) at [7:4], sy=0. The channel at [13:8] is deliberately
        // left as a placeholder: the owning transport ring always stamps its
        // configured channel immediately before publishing the descriptor.
        // The speed field is mandatory — omitting it transmits at S100 and
        // produces a header the device/analyzer treats as malformed.
        // Cross-validated with Linux: firewire/ohci.h:277-286 and
        // firewire/ohci.c:3377-3381.
        const uint32_t isochHeaderQ0 = (static_cast<uint32_t>(2 & 0x7) << 16) |
                                       (static_cast<uint32_t>(1 & 0x3) << 14) |
                                       (static_cast<uint32_t>(0xA & 0xF) << 4);
        meta.immediateHeader[0] = OSSwapHostToLittleInt32(isochHeaderQ0);

        // immediateData[1] = data_length (payload bytes) in bits [31:16]. The
        // CIP header is the first 8 bytes of the payload buffer and is shipped
        // by the OUTPUT_LAST descriptor — it does NOT belong in the packet
        // header immediate. Cross-validated with Linux:
        // firewire/ohci.h:287-288 and firewire/ohci.c:3383.
        meta.immediateHeader[1] = OSSwapHostToLittleInt32(
            static_cast<uint32_t>(packet.byteCount & 0xFFFF) << 16);

        // Arm this lap's arbitration before the commit that republishes the
        // slot. A previous lap's terminal phase must not survive into this
        // packet: the generation tag would reject it anyway, but leaving it
        // would make the slot permanently unclaimable rather than merely stale.
        // Ordered by the release-store of commitGeneration below, which is what
        // transport acquires before it reads this word.
        meta.payloadArbitration.store(
            ASFW::Isoch::MakeTxPayloadArbitration(
                ASFW::Isoch::ExpectedTxCommitGeneration(packet.packetIndex,
                                                        numSlots),
                ASFW::Isoch::TxPayloadArbitration::kNoAlternative),
            std::memory_order_relaxed);

        // Content is NOT inspected here. This point holds the armed image
        // (image 0), which carries the CIP header and default slots before any
        // PCM refill -- silence by construction. Measuring it reported
        // zeroPcm == dataPackets with maxAbs24 = 0 through an audibly playing
        // stream, which is how the inspector came to be trusted for a silence
        // diagnosis it could not make. What actually reaches the wire for a
        // content packet is the late image, so the inspection lives where that
        // image wins arbitration: see PublishLatePayload.

        // Compute expected generation and release-store it last.
        const uint64_t generation =
            ASFW::Isoch::ExpectedTxCommitGeneration(packet.packetIndex, numSlots);
        meta.commitGeneration.store(generation, std::memory_order_release);

        queueControl->committedEnd.store(packet.packetIndex + 1,
                                         std::memory_order_release);
        return true;
    }

    uint32_t SlotCount() const noexcept override {
        return numSlots;
    }
};

} // namespace ASFW::Audio
