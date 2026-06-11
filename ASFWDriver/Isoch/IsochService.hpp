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

#include "IsochReceiveContext.hpp"
#include "Transmit/IsochTransmitContext.hpp"
#include "../Common/DriverKitOwnership.hpp"

namespace ASFW::Audio::Runtime {
class IDirectAudioBindingSource;
}

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Driver {

class HardwareInterface;

class IsochService {
public:
    using TimingLossCallback = std::function<void(uint64_t guid)>;

    IsochService() = default;
    ~IsochService() = default;

    kern_return_t StartReceive(uint8_t channel,
                               HardwareInterface& hardware,
                               ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                               ASFW::Encoding::AudioWireFormat wireFormat = ASFW::Encoding::AudioWireFormat::kAM824,
                               uint32_t am824Slots = 0);

    kern_return_t StopReceive();

    kern_return_t StartTransmit(uint8_t channel,
                                HardwareInterface& hardware,
                                uint8_t sid);

    kern_return_t StopTransmit();

    kern_return_t BeginSplitDuplex(uint64_t guid);
    kern_return_t ReservePlaybackResources(uint64_t guid,
                                           IRM::IRMClient& irmClient,
                                           uint8_t channel,
                                           uint32_t bandwidthUnits);
    kern_return_t ReserveCaptureResources(uint64_t guid,
                                          IRM::IRMClient& irmClient,
                                          uint8_t channel,
                                          uint32_t bandwidthUnits);

    void StopAll();
    void SetTimingLossCallback(TimingLossCallback callback) noexcept;

    /**
     * @brief Allocates the shared payload slab, metadata ring, and control block.
     * @param numSlots The number of packet slots in the payload ring buffer.
     * @param maxPacketBytes Maximum size of a single packet payload in bytes.
     * @param interruptInterval Frequency of interrupts in packets.
     * @param outPayloadSlab Shared memory descriptor containing all packet payloads.
     * @param outMetadataRing Shared memory descriptor containing packet metadata.
     * @param outControlBlock Shared memory descriptor containing stream control states.
     */
    kern_return_t AllocateTxIsochResources(
        uint32_t numSlots,
        uint32_t maxPacketBytes,
        uint32_t interruptInterval,
        IOMemoryDescriptor** outPayloadSlab,
        IOMemoryDescriptor** outMetadataRing,
        IOMemoryDescriptor** outControlBlock);

    /**
     * @brief Releases all allocated shared transmit resources.
     */
    kern_return_t FreeTxIsochResources();

    /**
     * @brief Configure, initialize, and start the transmit DMA context stream.
     * @param channel The FireWire channel number (0-63).
     * @param speed The transmission speed code (S100, S200, S400).
     * @param hardware Reference to the low-level MMIO hardware interface.
     */
    kern_return_t StartTxStream(uint32_t channel, uint32_t speed, HardwareInterface& hardware);

    /**
     * @brief Stop the transmit DMA context stream.
     */
    kern_return_t StopTxStream();

    /**
     * @brief Query the current host time and FireWire cycle timer snapshot.
     * @param outHostTimeMid Monotonic host system time in ticks.
     * @param outCycleTimer Raw FireWire cycle timer value from hardware.
     * @param hardware Reference to the hardware interface.
     */
    kern_return_t GetCycleTimePair(uint64_t* outHostTimeMid, uint32_t* outCycleTimer, HardwareInterface& hardware);

    ASFW::Isoch::IsochReceiveContext* ReceiveContext() const { return isochReceiveContext_.get(); }
    ASFW::Isoch::IsochTransmitContext* TransmitContext() const { return isochTransmitContext_.get(); }

private:
    kern_return_t ClaimDuplexGuid(uint64_t guid);
    void RefreshReceiveTimingLossCallback() noexcept;
    void OnReceiveTimingLossDetected() noexcept;

    OSSharedPtr<ASFW::Isoch::IsochReceiveContext> isochReceiveContext_;
    std::unique_ptr<ASFW::Isoch::IsochTransmitContext> isochTransmitContext_;

    OSSharedPtr<IOBufferMemoryDescriptor> txPayloadSlab_{nullptr};
    OSSharedPtr<IOBufferMemoryDescriptor> txMetadataRing_{nullptr};
    OSSharedPtr<IOBufferMemoryDescriptor> txControlBlock_{nullptr};

    uint64_t activeGuid_{0};
    TimingLossCallback timingLossCallback_{};
    uint32_t interruptInterval_{8};

    struct ReservedDuplexResources {
        bool playbackActive{false};
        uint8_t playbackChannel{0xFF};
        uint32_t playbackBandwidthUnits{0};
        bool captureActive{false};
        uint8_t captureChannel{0xFF};
        uint32_t captureBandwidthUnits{0};

        void Reset() noexcept {
            playbackActive = false;
            playbackChannel = 0xFF;
            playbackBandwidthUnits = 0;
            captureActive = false;
            captureChannel = 0xFF;
            captureBandwidthUnits = 0;
        }
    } reserved_{};
};

} // namespace ASFW::Driver
