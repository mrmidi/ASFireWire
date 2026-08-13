#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include "../Common/DriverKitOwnership.hpp"
#include "IsochReceiveContext.hpp"
#include "Transmit/IsochTransmitContext.hpp"

namespace ASFW::Driver {

class HardwareInterface;

class IsochService {
  public:
    using TxPreparationCallback = std::function<void(uint64_t generation)>;

    IsochService() = default;
    ~IsochService() = default;

    // Maximum isochronous contexts per direction. Context 0 is primary;
    // contexts 1+ are independent packet streams. One OHCI IR/IT hardware
    // context backs each index.
    static constexpr uint32_t kMaxStreamsPerDirection = 4;

    kern_return_t StartReceive(uint8_t channel, HardwareInterface& hardware,
                               ASFW::Isoch::IsochReceiveCallback packetCallback = nullptr);
    kern_return_t PrepareReceive(uint8_t channel, HardwareInterface& hardware,
                                 ASFW::Isoch::IsochReceiveCallback packetCallback = nullptr);
    // Prepare an additional receive stream on its own OHCI IR context. Content
    // layout and destination offsets belong to the caller-owned consumer.
    kern_return_t PrepareReceiveStream(
        uint32_t streamIndex, uint8_t channel, HardwareInterface& hardware);
    kern_return_t StartPreparedReceive();
    // Schedule every prepared IR context on one bus-cycle boundary. This is a
    // transport primitive: the caller supplies an opaque OHCI cycle timer and
    // retains ownership of policy for why that cycle was chosen.
    kern_return_t StartPreparedReceiveAtCycle(uint32_t cycleTimer);

    // Starts stream 0 with a caller-owned, content-side packet consumer. The
    // transport remains payload-opaque; the consumer must stay alive until
    // StopPacketReceive() succeeds.
    kern_return_t StartPacketReceive(
        uint8_t channel, HardwareInterface& hardware,
        ASFW::Isoch::IIsochReceiveConsumer* consumer);
    [[nodiscard]] kern_return_t StopPacketReceive(
        ASFW::Isoch::IIsochReceiveConsumer* consumer);

    kern_return_t StopReceive();

    kern_return_t StartTransmit(uint8_t channel, HardwareInterface& hardware, uint8_t sid);
    kern_return_t PrepareTransmit(uint8_t channel, HardwareInterface& hardware, uint8_t sid);
    // Prepare an additional transmit stream on its own OHCI IT context. The
    // caller supplies an opaque shared packet queue separately.
    kern_return_t PrepareTransmitStream(uint32_t streamIndex, uint8_t channel,
                                        HardwareInterface& hardware, uint8_t sid);
    kern_return_t StartPreparedTransmit();

    kern_return_t StopTransmit();

    // Do not tear down a producer/consumer binding after a failed stop: an
    // ACTIVE OHCI context may still DMA into that mapping.
    [[nodiscard]] kern_return_t StopAll();
    [[nodiscard]] bool HardwareGone() const noexcept;
    // The caller owns the consumer and must detach it only after StopReceive()
    // has succeeded (ACTIVE clear). Stream 0 is the master; streams 1+ are
    // secondary hardware contexts.
    void SetReceiveConsumer(uint32_t streamIndex,
                            ASFW::Isoch::IIsochReceiveConsumer* consumer) noexcept;
    void SetTxPreparationCallback(TxPreparationCallback callback) noexcept;

    /**
     * @brief Allocates the shared payload slab, metadata ring, and control block.
     * @param numSlots The number of packet slots in the payload ring buffer.
     * @param maxPacketBytes Maximum size of a single packet payload in bytes.
     * @param interruptInterval Frequency of interrupts in packets.
     * @param outPayloadSlab Shared memory descriptor containing all packet payloads.
     * @param outMetadataRing Shared memory descriptor containing packet metadata.
     * @param outControlBlock Shared memory descriptor containing stream control states.
     */
    kern_return_t AllocateTxIsochResources(uint32_t streamIndex, uint32_t numSlots,
                                           uint32_t maxPacketBytes, uint32_t interruptInterval,
                                           IOMemoryDescriptor** outPayloadSlab,
                                           IOMemoryDescriptor** outMetadataRing,
                                           IOMemoryDescriptor** outControlBlock);

    /**
     * @brief Releases all allocated shared transmit resources (every stream).
     */
    kern_return_t FreeTxIsochResources();

    /**
     * @brief Query the current host time and FireWire cycle timer snapshot.
     * @param outHostTimeMid Monotonic host system time in ticks.
     * @param outCycleTimer Raw FireWire cycle timer value from hardware.
     * @param hardware Reference to the hardware interface.
     */
    kern_return_t GetCycleTimePair(uint64_t* outHostTimeMid, uint32_t* outCycleTimer,
                                   HardwareInterface& hardware);

    ASFW::Isoch::IsochReceiveContext* ReceiveContext() const { return isochReceiveContext_.get(); }
    ASFW::Isoch::IsochTransmitContext* TransmitContext() const {
        return isochTransmitContext_.get();
    }

    // Per-stream accessors: index 0 == master, index 1+ == secondary streams.
    ASFW::Isoch::IsochReceiveContext* ReceiveContext(uint32_t streamIndex) const {
        if (streamIndex == 0)
            return isochReceiveContext_.get();
        if (streamIndex < kMaxStreamsPerDirection)
            return secondaryReceiveContexts_[streamIndex - 1].get();
        return nullptr;
    }
    ASFW::Isoch::IsochTransmitContext* TransmitContext(uint32_t streamIndex) const {
        if (streamIndex == 0)
            return isochTransmitContext_.get();
        if (streamIndex < kMaxStreamsPerDirection)
            return secondaryTransmitContexts_[streamIndex - 1].get();
        return nullptr;
    }

  private:
    // Primary receive/transmit contexts. Content-side policy may assign a
    // special role to index 0, but transport does not know that policy.
    std::unique_ptr<ASFW::Isoch::IsochReceiveContext> isochReceiveContext_;
    std::unique_ptr<ASFW::Isoch::IsochTransmitContext> isochTransmitContext_;

    // Secondary streams [1 .. kMaxStreamsPerDirection). Index i here maps to
    // stream (i + 1); each runs on its own OHCI context (contextIndex == stream).
    std::unique_ptr<ASFW::Isoch::IsochReceiveContext>
        secondaryReceiveContexts_[kMaxStreamsPerDirection - 1];
    std::unique_ptr<ASFW::Isoch::IsochTransmitContext>
        secondaryTransmitContexts_[kMaxStreamsPerDirection - 1];

    ASFW::Isoch::IIsochReceiveConsumer*
        receiveConsumers_[kMaxStreamsPerDirection]{nullptr, nullptr, nullptr, nullptr};

    // Per-stream opaque TX shared resources. Each IT context DMAs its own slab;
    // transport neither produces nor interprets the bytes.
    OSSharedPtr<IOBufferMemoryDescriptor> txPayloadSlab_[kMaxStreamsPerDirection]{};
    OSSharedPtr<IOBufferMemoryDescriptor> txMetadataRing_[kMaxStreamsPerDirection]{};
    OSSharedPtr<IOBufferMemoryDescriptor> txControlBlock_[kMaxStreamsPerDirection]{};

    TxPreparationCallback txPreparationCallback_{};
    uint32_t interruptInterval_{8};

    HardwareInterface* hardware_{nullptr};
    void UpdateStreamingActiveState() noexcept;
};

} // namespace ASFW::Driver
