#include "FakeIsochTxSlotProvider.hpp"

namespace ASFW::Lab {

// Dumb storage standing in for the OHCI IT DMA ring: 256 slots × 512 B
// (mirrors the production ring geometry). Acquiring a ring position
// invalidates its previous publication — the analog of the hardware ring
// reusing a descriptor — and PublishSlot records the PreparedTxPacket
// verbatim so tests (and the Step 6 verifier) can inspect exactly what the
// engine claims to have put on the wire.

using Protocols::Audio::AMDTP::PreparedTxPacket;
using Protocols::Audio::AMDTP::TxPacketSlotView;

void FakeIsochTxSlotProvider::Reset() noexcept {
    for (FakeSlot& slot : slots_) {
        for (uint8_t& byte : slot.bytes) {
            byte = 0;
        }
        slot.published = PreparedTxPacket{};
        slot.hasPublishedPacket = false;
    }
}

bool FakeIsochTxSlotProvider::AcquireWritableSlot(
    uint32_t packetIndex, TxPacketSlotView& outSlot) noexcept {
    FakeSlot& slot = slots_[packetIndex % kSlotCount];
    slot.hasPublishedPacket = false; // ring reuse evicts the old occupant
    outSlot = TxPacketSlotView{packetIndex, slot.bytes, kSlotCapacityBytes};
    return true;
}

void FakeIsochTxSlotProvider::PublishSlot(
    const PreparedTxPacket& packet) noexcept {
    FakeSlot& slot = slots_[packet.packetIndex % kSlotCount];
    slot.published = packet;
    slot.hasPublishedPacket = true;
}

uint32_t FakeIsochTxSlotProvider::SlotCount() const noexcept {
    return kSlotCount;
}

const uint8_t* FakeIsochTxSlotProvider::SlotBytes(
    uint32_t packetIndex) const noexcept {
    return slots_[packetIndex % kSlotCount].bytes;
}

uint8_t* FakeIsochTxSlotProvider::SlotBytes(uint32_t packetIndex) noexcept {
    return slots_[packetIndex % kSlotCount].bytes;
}

const PreparedTxPacket* FakeIsochTxSlotProvider::PublishedPacket(
    uint32_t packetIndex) const noexcept {
    const FakeSlot& slot = slots_[packetIndex % kSlotCount];
    if (!slot.hasPublishedPacket || slot.published.packetIndex != packetIndex) {
        return nullptr;
    }
    return &slot.published;
}

} // namespace ASFW::Lab
