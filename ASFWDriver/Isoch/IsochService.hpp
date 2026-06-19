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
#include "Receive/DVCaptureSink.hpp"
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
    using TxPreparationCallback = std::function<void(uint64_t generation)>;
    using ZtsAnchorReadyCallback = std::function<void(uint64_t generation)>;

    IsochService() = default;
    ~IsochService() = default;

    kern_return_t StartReceive(uint8_t channel,
                               HardwareInterface& hardware,
                               ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                               ASFW::Encoding::AudioWireFormat wireFormat = ASFW::Encoding::AudioWireFormat::kAM824,
                               uint32_t am824Slots = 0,
                               ASFW::Isoch::IsochReceiveCallback packetCallback = nullptr);
    kern_return_t PrepareReceive(uint8_t channel,
                                 HardwareInterface& hardware,
                                 ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                                 ASFW::Encoding::AudioWireFormat wireFormat = ASFW::Encoding::AudioWireFormat::kAM824,
                                 uint32_t am824Slots = 0,
                                 ASFW::Isoch::IsochReceiveCallback packetCallback = nullptr);
    kern_return_t StartPreparedReceive();

    kern_return_t StopReceive();

    // Minimal DV (IEC 61883-2) capture tap: starts IR on the given channel with
    // no audio binding and streams raw DIF chunks into a shared ring the app
    // maps via CopyClientMemoryForType(type=1).
    kern_return_t StartDVCapture(uint8_t channel, HardwareInterface& hardware);
    kern_return_t StopDVCapture();
    kern_return_t CopyDVCaptureMemory(uint64_t* options, IOMemoryDescriptor** memory) const;

    kern_return_t StartTransmit(uint8_t channel,
                                HardwareInterface& hardware,
                                uint8_t sid);
    kern_return_t PrepareTransmit(uint8_t channel,
                                  HardwareInterface& hardware,
                                  uint8_t sid);
    kern_return_t StartPreparedTransmit();

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
    void SetTxPreparationCallback(TxPreparationCallback callback) noexcept;
    void SetZtsAnchorReadyCallback(ZtsAnchorReadyCallback callback) noexcept;

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
    void StartDeferredTransmitIfReady() noexcept;

    OSSharedPtr<ASFW::Isoch::IsochReceiveContext> isochReceiveContext_;
    std::unique_ptr<ASFW::Isoch::IsochTransmitContext> isochTransmitContext_;

    // DV capture shared ring (see Receive/DVCaptureSink.hpp)
    struct DVRingMapping {
        OSSharedPtr<IOBufferMemoryDescriptor> memory{};
        OSSharedPtr<IOMemoryMap> map{};
        uint64_t bytes{0};

        void Reset() noexcept {
            map.reset();
            memory.reset();
            bytes = 0;
        }

        [[nodiscard]] void* BaseAddress() const noexcept {
            return map ? reinterpret_cast<void*>(static_cast<uintptr_t>(map->GetAddress())) : nullptr;
        }
    };

    DVRingMapping dvRing_{};
    ASFW::Isoch::Rx::DVCaptureSink dvSink_{};
    bool dvCaptureActive_{false};

    OSSharedPtr<IOBufferMemoryDescriptor> txPayloadSlab_{nullptr};
    OSSharedPtr<IOBufferMemoryDescriptor> txMetadataRing_{nullptr};
    OSSharedPtr<IOBufferMemoryDescriptor> txControlBlock_{nullptr};

    uint64_t activeGuid_{0};
    TimingLossCallback timingLossCallback_{};
    TxPreparationCallback txPreparationCallback_{};
    ZtsAnchorReadyCallback ztsAnchorReadyCallback_{};
    bool txStartPending_{false};
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
