#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Runtime {

/// Audio-owned, producer-side payload diagnostics.  This deliberately lives
/// beside the packetizer rather than at the IT DMA refill: transport must not
/// know CIP or AM824 layout.  All counters are drained through ring-only log
/// events, never unified logging on the packet hot path.
struct TxWirePayloadObservation final {
    bool firstInfo{false};
    bool dropout{false};
    uint32_t infoQuads{0};
    uint32_t maxAbs24{0};
    uint32_t slotMask{0};
    uint32_t lastInfoQuad{0};
};

struct TxWirePayloadTelemetry final {
    std::atomic<uint64_t> dataPackets{0};
    std::atomic<uint64_t> zeroPcmPackets{0};

    // Late-payload outcomes, attributed rather than collapsed. The content
    // inspector only runs for a won offer whose packet carries payload, so a
    // silent inspector has three possible causes that present identically:
    // the offer never won, it won on a header-only packet, or it won on a data
    // packet that was genuinely zero. Counting the first two separates them.
    std::atomic<uint64_t> lateOffersWon{0};
    std::atomic<uint64_t> lateWonHeaderOnly{0};

    /// AM824 slots per data block, published once when the stream is
    /// configured. Observe() needs it to turn a flat quadlet index into a slot
    /// index; zero means "not published", and the slot mask is then left alone
    /// rather than filled with a guess.
    std::atomic<uint32_t> dataBlockQuadlets{0};

    /// Bit per slot index that has ever carried a non-zero, non-idle quadlet.
    /// This is what says WHICH channels the stream is actually filling, as
    /// opposed to how many -- the difference between "stereo is present" and
    /// "stereo is present on the pair the device routes to the outputs".
    std::atomic<uint32_t> slotMask{0};
    std::atomic<uint64_t> infoQuads{0};
    std::atomic<uint64_t> pcmDropouts{0};
    std::atomic<uint32_t> maxAbs24{0};
    std::atomic<uint32_t> lastInfoQuad{0};
    std::atomic<uint64_t> firstInfoPacketIndex{0};

    void Reset() noexcept {
        dataPackets.store(0, std::memory_order_relaxed);
        zeroPcmPackets.store(0, std::memory_order_relaxed);
        lateOffersWon.store(0, std::memory_order_relaxed);
        lateWonHeaderOnly.store(0, std::memory_order_relaxed);
        slotMask.store(0, std::memory_order_relaxed);
        infoQuads.store(0, std::memory_order_relaxed);
        pcmDropouts.store(0, std::memory_order_relaxed);
        maxAbs24.store(0, std::memory_order_relaxed);
        lastInfoQuad.store(0, std::memory_order_relaxed);
        firstInfoPacketIndex.store(0, std::memory_order_relaxed);
        lastPacketHadInfo_ = false;
        firstInfoSeen_ = false;
    }

    [[nodiscard]] TxWirePayloadObservation Observe(
        uint64_t packetIndex, const uint8_t* packetBytes,
        uint32_t payloadLength) noexcept {
        constexpr uint32_t kCipHeaderBytes = 8;
        constexpr uint32_t kIdleSlotWord = 0x80000000u;
        TxWirePayloadObservation observation{};
        if (packetBytes == nullptr || payloadLength <= kCipHeaderBytes) {
            return observation;
        }

        dataPackets.fetch_add(1, std::memory_order_relaxed);
        const uint8_t* quadBytes = packetBytes + kCipHeaderBytes;
        const uint32_t quadCount = (payloadLength - kCipHeaderBytes) / 4;
        const uint32_t blockQuads =
            dataBlockQuadlets.load(std::memory_order_relaxed);
        uint32_t observedSlots = 0;
        for (uint32_t index = 0; index < quadCount; ++index, quadBytes += 4) {
            const uint32_t quad =
                (static_cast<uint32_t>(quadBytes[0]) << 24) |
                (static_cast<uint32_t>(quadBytes[1]) << 16) |
                (static_cast<uint32_t>(quadBytes[2]) << 8) |
                static_cast<uint32_t>(quadBytes[3]);
            if (quad == 0 || quad == kIdleSlotWord) continue;
            if (blockQuads != 0) {
                const uint32_t slot = index % blockQuads;
                if (slot < 32) observedSlots |= (1u << slot);
            }
            ++observation.infoQuads;
            observation.lastInfoQuad = quad;
            const int32_t sample24 = static_cast<int32_t>(quad << 8) >> 8;
            const uint32_t magnitude = static_cast<uint32_t>(
                sample24 < 0 ? -static_cast<int64_t>(sample24) : sample24);
            observation.maxAbs24 = magnitude > observation.maxAbs24
                ? magnitude : observation.maxAbs24;
        }

        if (observation.infoQuads == 0) {
            zeroPcmPackets.fetch_add(1, std::memory_order_relaxed);
            observation.dropout = lastPacketHadInfo_;
            if (observation.dropout) {
                pcmDropouts.fetch_add(1, std::memory_order_relaxed);
            }
            lastPacketHadInfo_ = false;
            return observation;
        }

        if (observedSlots != 0) {
            slotMask.fetch_or(observedSlots, std::memory_order_relaxed);
        }
        observation.slotMask = observedSlots;
        infoQuads.fetch_add(observation.infoQuads, std::memory_order_relaxed);
        lastInfoQuad.store(observation.lastInfoQuad, std::memory_order_relaxed);
        uint32_t previous = maxAbs24.load(std::memory_order_relaxed);
        while (observation.maxAbs24 > previous &&
               !maxAbs24.compare_exchange_weak(
                   previous, observation.maxAbs24, std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
        observation.firstInfo = !firstInfoSeen_;
        if (observation.firstInfo) {
            firstInfoPacketIndex.store(packetIndex, std::memory_order_relaxed);
            firstInfoSeen_ = true;
        }
        lastPacketHadInfo_ = true;
        return observation;
    }

private:
    bool lastPacketHadInfo_{false};
    bool firstInfoSeen_{false};
};

} // namespace ASFW::Audio::Runtime
