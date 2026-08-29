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

    /// Where transport has already bound packets to descriptors. A packet at or
    /// beyond this index can still take a late payload; below it the bytes are
    /// frozen. End-exclusive, monotonic within a stream generation.
    [[nodiscard]] virtual uint64_t MappedEnd() const noexcept = 0;

    /// A second writable image for an already-published packet. Fails once the
    /// packet is frozen, so a caller cannot write bytes transport may be
    /// binding.
    [[nodiscard]] virtual bool AcquireLatePayloadSlot(
        uint32_t packetIndex, TxPacketSlotView& outSlot) noexcept = 0;

    /// Offer the late image for this packet. Transport takes it only if it maps
    /// the slot afterwards; losing that race is normal and not an error, which
    /// is why this reports publication, not acceptance.
    [[nodiscard]] virtual bool PublishLatePayload(
        uint32_t packetIndex) noexcept = 0;
};

} // namespace ASFW::Protocols::Audio::AMDTP
