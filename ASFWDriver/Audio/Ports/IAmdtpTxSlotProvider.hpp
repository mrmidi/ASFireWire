#pragma once

#include "../Wire/AMDTP/AmdtpTypes.hpp"

namespace ASFW::Protocols::Audio::AMDTP {

class IAmdtpTxSlotProvider {
public:
    virtual ~IAmdtpTxSlotProvider() = default;

    virtual bool AcquireWritableSlot(uint32_t packetIndex,
                                     TxPacketSlotView& outSlot) noexcept = 0;

    [[nodiscard]] virtual bool PublishSlot(
        const PreparedTxPacket& packet) noexcept = 0;

    virtual uint32_t SlotCount() const noexcept = 0;

    /// Where transport has made the payload choice final. A packet at or beyond
    /// this index can still take a late payload even when it is already bound;
    /// below it the armed image must remain unchanged. End-exclusive and
    /// monotonic within a stream generation.
    [[nodiscard]] virtual uint64_t FinalizedEnd() const noexcept = 0;

    /// A second writable image for an already-published packet. Fails once the
    /// packet is frozen, so a caller cannot write bytes transport may be
    /// binding.
    [[nodiscard]] virtual bool AcquireLatePayloadSlot(
        uint32_t packetIndex, TxPacketSlotView& outSlot) noexcept = 0;

    /// Offer the late image for this packet. Transport may take it while
    /// initially binding the descriptor or by atomically repointing an already
    /// bound mutable tail. Losing the finality race is normal.
    [[nodiscard]] virtual bool PublishLatePayload(
        uint32_t packetIndex) noexcept = 0;
};

} // namespace ASFW::Protocols::Audio::AMDTP
