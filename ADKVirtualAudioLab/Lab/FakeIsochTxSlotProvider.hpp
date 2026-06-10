#pragma once

#include "../Ports/IAmdtpTxSlotProvider.hpp"

#include <array>
#include <cstdint>

namespace ASFW::Lab {

class FakeIsochTxSlotProvider final
    : public Protocols::Audio::AMDTP::IAmdtpTxSlotProvider {
public:
    static constexpr uint32_t kSlotCount = 512;
    static constexpr uint32_t kSlotCapacityBytes = 512;

    FakeIsochTxSlotProvider() noexcept = default;

    void Reset() noexcept;

    bool AcquireWritableSlot(
        uint32_t packetIndex,
        Protocols::Audio::AMDTP::TxPacketSlotView& outSlot) noexcept override;

    void PublishSlot(
        const Protocols::Audio::AMDTP::PreparedTxPacket& packet) noexcept override;

    uint32_t SlotCount() const noexcept override;

    const uint8_t* SlotBytes(uint32_t packetIndex) const noexcept;
    uint8_t* SlotBytes(uint32_t packetIndex) noexcept;

    const Protocols::Audio::AMDTP::PreparedTxPacket* PublishedPacket(
        uint32_t packetIndex) const noexcept;

private:
    struct FakeSlot final {
        alignas(16) uint8_t bytes[kSlotCapacityBytes]{};
        Protocols::Audio::AMDTP::PreparedTxPacket published{};
        bool hasPublishedPacket{false};
    };

    std::array<FakeSlot, kSlotCount> slots_{};
};

} // namespace ASFW::Lab