#pragma once

#include "../Wire/AMDTP/AmdtpTypes.hpp"

#include <cstdint>
#include <span>

namespace ASFW::Protocols::Audio::AMDTP {

class IAmdtpTxSlotProvider {
public:
    virtual ~IAmdtpTxSlotProvider() = default;

    virtual bool AcquireWritableSlot(uint64_t packetIndex,
                                     TxPacketSlotView& outSlot) noexcept = 0;

    [[nodiscard]] virtual bool PublishSlot(
        const PreparedTxPacket& packet) noexcept = 0;

    virtual uint32_t SlotCount() const noexcept = 0;
};

/// Audio-side observer of a TX packet at its release: the slot provider calls
/// it from PublishSlot with the packet's final bytes, immediately before the
/// release commit hands the slot to transport. It reads what the packetizer and
/// the timing stamper wrote (CIP header, device timing); host PCM is filled
/// later and is not there yet. Content inspection stays on the Audio side of
/// the seam; transport only ever sees opaque bytes.
///
/// Called on the TX preparation queue, once per published packet; must not
/// block, allocate or log.
// Audio-side execution-time projection; consumes the transport's neutral
// completion contract without exposing its queue mechanics to content codecs.
class ITxPacketExecutionTimeline {
public:
    virtual ~ITxPacketExecutionTimeline() = default;
    [[nodiscard]] virtual bool AnchorForPacket(uint64_t packetIndex, int64_t& ticks) const noexcept = 0;
};

class ITxPacketReleaseObserver {
public:
    virtual ~ITxPacketReleaseObserver() = default;

    virtual void OnRelease(const PreparedTxPacket& packet,
                           std::span<const uint8_t> packetBytes) noexcept = 0;
};

} // namespace ASFW::Protocols::Audio::AMDTP
