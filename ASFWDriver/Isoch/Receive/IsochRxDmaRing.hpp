// IsochRxDmaRing.hpp
// ASFW - Low-level OHCI IR DMA ring engine (generic, no audio semantics).

#pragma once

#include "../Memory/IIsochDMAMemory.hpp"
#include "../../Shared/Rings/BufferRing.hpp"
#include "../../Hardware/OHCIDescriptors.hpp"
#include "../../Common/BarrierUtils.hpp"

#include <cstddef>
#include <cstdint>

namespace ASFW::Isoch::Rx {

class IsochRxDmaRing final {
public:
    using OHCIDescriptor = Async::HW::OHCIDescriptor;

    struct CompletedPacket final {
        uint32_t descriptorIndex{0};
        uint16_t xferStatus{0};
        uint16_t resCount{0};
        uint16_t actualLength{0};
        const uint8_t* payload{nullptr};
    };

    [[nodiscard]] kern_return_t SetupRings(Memory::IIsochDMAMemory& dma,
                                          size_t numDescriptors,
                                          size_t maxPacketSizeBytes) noexcept;

    void ResetForStart() noexcept { lastProcessedIndex_ = 0; }

    [[nodiscard]] uint32_t InitialCommandPtrWord() const noexcept;

    template <typename Handler>
    [[nodiscard]] uint32_t DrainCompleted(Memory::IIsochDMAMemory& dma,
                                          Handler&& onPacket) noexcept {
        const uint32_t capacity = static_cast<uint32_t>(bufferRing_.Capacity());
        if (capacity == 0 || maxPacketSizeBytes_ == 0) {
            return 0;
        }

        const uint16_t reqCount = static_cast<uint16_t>(maxPacketSizeBytes_);

        uint32_t processed = 0;
        uint32_t idx = lastProcessedIndex_;

        for (uint32_t scanned = 0; scanned < capacity; ++scanned) {
            auto* desc = bufferRing_.GetDescriptor(idx);
            if (!desc) {
                break;
            }

            dma.FetchFromDevice(desc, sizeof(*desc));

            const uint32_t status = desc->statusWord;
            const uint16_t xferStatus = static_cast<uint16_t>(status >> 16);
            const uint16_t resCount = static_cast<uint16_t>(status);
            // A residual byte count is progress, not ownership transfer.
            // Cross-checked with Linux ohci.c:2781-2813: packet-per-buffer
            // retirement waits for transfer_status before accessing payload.
            if (xferStatus == 0) {
                break;
            }

            // Completion observation must precede payload reads. Linux
            // ohci.c:2781-2813 synchronizes RX payload for the CPU here.
            // FetchFromDevice's OSSynchronizeIO alone is DMB ST on arm64,
            // even for cache-inhibited mappings; it does not order loads.
            dma.ReceiveOwnershipBarrier();
            const uint16_t actualLength = (resCount <= reqCount) ? static_cast<uint16_t>(reqCount - resCount) : 0;
            auto* payloadVA = bufferRing_.GetElementVA(idx);
            if (payloadVA && actualLength > 0) {
                dma.FetchFromDevice(payloadVA, actualLength);
            }

            CompletedPacket packet{
                .descriptorIndex = idx,
                .xferStatus = xferStatus,
                .resCount = resCount,
                .actualLength = actualLength,
                .payload = payloadVA,
            };
            onPacket(packet);

            // Finish payload reads before clearing the completion marker.
            // Linux queues payload back for the device only after consumption
            // (amdtp-stream.c:1343-1356; ohci.c:3370-3392). This orders CPU
            // accesses; the circular program still has no hardware stop fence.
            dma.ReceiveOwnershipBarrier();
            Async::HW::AR_init_status(*desc, reqCount);
            dma.PublishToDevice(desc, sizeof(*desc));

            idx = (idx + 1) % capacity;
            lastProcessedIndex_ = idx;
            ++processed;
        }

        if (processed > 0) {
            ::ASFW::Driver::WriteBarrier();
        }

        return processed;
    }

    // Debug/test helpers.
    [[nodiscard]] size_t Capacity() const noexcept { return bufferRing_.Capacity(); }

    [[nodiscard]] OHCIDescriptor* DescriptorAt(size_t index) noexcept { return bufferRing_.GetDescriptor(index); }

    [[nodiscard]] uint8_t* PayloadVA(size_t index) const noexcept {
        return bufferRing_.GetElementVA(index);
    }

    [[nodiscard]] uint32_t Descriptor0IOVA() const noexcept;

private:
    Shared::BufferRing bufferRing_{};
    size_t maxPacketSizeBytes_{0};
    uint32_t lastProcessedIndex_{0};
};

} // namespace ASFW::Isoch::Rx
