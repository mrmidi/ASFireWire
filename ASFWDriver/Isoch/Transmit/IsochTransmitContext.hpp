// IsochTransmitContext.hpp
// ASFW - Isochronous Transmit Context
//
// Manages the IT context for transmitting audio via FireWire 1394.
// Phase 2: Real Hardware IT DMA
//
// Data flow:
//   CoreAudio → IOOperationHandler → RingBuffer → PacketAssembler → DMA → Wire
//                                                    ↑
//                                              (owned by this class)

#pragma once

#include "../Encoding/PacketAssembler.hpp"
#include "../Memory/IIsochDMAMemory.hpp"
#include "../../Shared/Rings/DescriptorRing.hpp"
#include "../../Shared/TxSharedQueue.hpp"
#include "../../Hardware/RegisterMap.hpp" // For Register32

#include "../../Logging/Logging.hpp"
#include <cstdint>
#include <memory>

namespace ASFW {

namespace Driver { class HardwareInterface; }

namespace Isoch {

enum class ITState {
    Unconfigured,
    Configured,
    Running,
    Stopped
};

class IsochTransmitContext {
public:
    using State = ITState;

    static constexpr uint32_t kBlocksPerPacket = 3;
    static constexpr uint32_t kNumPackets = 84;
    static constexpr uint32_t kRingBlocks = kNumPackets * kBlocksPerPacket;
    static constexpr uint32_t kMaxPacketSize = 4096;

    static constexpr uint32_t kNumDescriptors = kRingBlocks;
    static constexpr size_t kDescriptorRingSize = kRingBlocks * 16;
    static constexpr size_t kPayloadBufferSize = kNumPackets * kMaxPacketSize;

    static_assert(kRingBlocks % kBlocksPerPacket == 0, "Ring must be divisible by blocks per packet");
    static_assert(kDescriptorRingSize <= 4096, "Descriptor ring must fit in one page");
    static_assert(kBlocksPerPacket == 3, "Z must be 3 for OMI(2)+OL(1)");
    static constexpr uint32_t kPacketsPerBatch = 8;

    IsochTransmitContext() noexcept = default;
    
    static std::unique_ptr<IsochTransmitContext> Create(
        Driver::HardwareInterface* hw,
        std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory) noexcept;

    kern_return_t Configure(uint8_t channel, uint8_t sid) noexcept;
    kern_return_t Start() noexcept;
    void Stop() noexcept;
    
    void Poll() noexcept;

    void HandleInterrupt() noexcept;

    State GetState() const noexcept { return state_; }
    Encoding::StereoAudioRingBuffer& RingBuffer() noexcept { return assembler_.ringBuffer(); }

    void SetSharedTxQueue(void* base, uint64_t bytes) noexcept;

    uint32_t SharedTxFillLevelFrames() const noexcept;
    
    uint64_t PacketsAssembled() const noexcept { return packetsAssembled_; }
    uint64_t DataPackets() const noexcept { return dataPackets_; }
    uint64_t NoDataPackets() const noexcept { return noDataPackets_; }
    uint64_t UnderrunCount() const noexcept { return assembler_.underrunCount(); }
    uint32_t BufferFillLevel() const noexcept { return assembler_.bufferFillLevel(); }
    
    void LogStatistics() const noexcept;

    void DumpPayloadBuffers(uint32_t numPackets = 4) const noexcept;

    void PrimeOnly() noexcept;

    void DumpDescriptorRing(uint32_t startPacket = 0, uint32_t numPackets = 8) const noexcept;

private:
    kern_return_t SetupRings() noexcept;
    void PrimeRing() noexcept;
    void RefillRing() noexcept;
    void WakeHardware() noexcept;
    void DumpAtCmdPtr() const noexcept;


    Encoding::PacketAssembler assembler_;
    Shared::TxSharedQueueSPSC sharedTxQueue_;
    State state_{State::Unconfigured};
    uint8_t channel_{0};
    uint8_t contextIndex_{0};
    
    Driver::HardwareInterface* hardware_{nullptr};
    std::shared_ptr<Memory::IIsochDMAMemory> dmaMemory_;

    Shared::DescriptorRing descriptorRing_;
    Shared::DMARegion descRegion_{};
    Shared::DMARegion bufRegion_{};

    uint32_t payloadIndex_{0};

    uint32_t softwareFillIndex_{0};
    uint32_t lastHwPacketIndex_{0};

    uint64_t packetsAssembled_{0};
    uint64_t dataPackets_{0};
    uint64_t noDataPackets_{0};
    uint64_t tickCount_{0};
    uint64_t interruptCount_{0};
};

} // namespace Isoch
} // namespace ASFW
