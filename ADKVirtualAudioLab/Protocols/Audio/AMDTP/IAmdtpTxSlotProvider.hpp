#pragma once

#include "AmdtpTypes.hpp"

namespace ASFW::Protocols::Audio::AMDTP {

class IAmdtpTxSlotProvider {
public:
    virtual ~IAmdtpTxSlotProvider() = default;

    virtual bool AcquireWritableSlot(uint32_t packetIndex,
                                     TxPacketSlotView& outSlot) noexcept = 0;

    virtual void PublishSlot(const PreparedTxPacket& packet) noexcept = 0;

    virtual uint32_t SlotCount() const noexcept = 0;
};

} // namespace ASFW::Protocols::Audio::AMDTP