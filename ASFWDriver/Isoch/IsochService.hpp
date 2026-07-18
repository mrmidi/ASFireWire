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
#include "Receive/DVCaptureSink.hpp"
#include "Transmit/IsochTransmitContext.hpp"

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

    // Maximum isochronous streams per direction. Stream 0 is the "master"
    // (owns the clock/ZTS/replay role); streams 1+ are secondary slices used by
    // multi-stream DICE devices (e.g. Venice F32 = 2×16 channels). A single OHCI
    // IR/IT hardware context backs each stream (contextIndex == streamIndex).
    static constexpr uint32_t kMaxStreamsPerDirection = 4;

    kern_return_t StartReceive(uint8_t channel, HardwareInterface& hardware,
                               ASFW::Isoch::IsochReceiveCallback packetCallback = nullptr);
    kern_return_t PrepareReceive(uint8_t channel, HardwareInterface& hardware,
                                 ASFW::Isoch::IsochReceiveCallback packetCallback = nullptr);
    // Prepare a secondary capture stream (streamIndex >= 1) on its own OHCI IR
    // context. channelOffset is the first host input channel this stream writes
    // (e.g. 16 for the second 16-ch slice of a 32-ch device); it is recorded for
    // the audio-engine de-interleave pass and not yet applied to the decoder.
    kern_return_t PrepareReceiveStream(
        uint32_t streamIndex, uint8_t channel, HardwareInterface& hardware,
        uint32_t channelOffset, uint32_t streamChannels);
    kern_return_t StartPreparedReceive();

    kern_return_t StopReceive();

    [[nodiscard]] uint32_t CaptureStreamChannelOffset(uint32_t streamIndex) const noexcept {
        return (streamIndex < kMaxStreamsPerDirection) ? captureChannelOffset_[streamIndex] : 0;
    }

    // Minimal DV (IEC 61883-2) capture tap: starts IR on the given channel with
    // no audio binding and streams raw DIF chunks into a shared ring the app
    // maps via CopyClientMemoryForType(type=1).
    kern_return_t StartDVCapture(uint8_t channel, HardwareInterface& hardware);
    kern_return_t StopDVCapture();
    kern_return_t CopyDVCaptureMemory(uint64_t* options, IOMemoryDescriptor** memory) const;

    kern_return_t StartTransmit(uint8_t channel, HardwareInterface& hardware, uint8_t sid);
    kern_return_t PrepareTransmit(uint8_t channel, HardwareInterface& hardware, uint8_t sid);
    // Prepare a secondary playback stream (streamIndex >= 1) on its own OHCI IT
    // context. The shared payload slab for the secondary stream is wired by the
    // audio-engine pass via SetSecondaryTransmitSharedMemory(); this only
    // creates and configures the hardware context.
    kern_return_t PrepareTransmitStream(uint32_t streamIndex, uint8_t channel,
                                        HardwareInterface& hardware, uint8_t sid);
    kern_return_t StartPreparedTransmit();

    kern_return_t StopTransmit();

    kern_return_t BeginSplitDuplex(uint64_t guid);
    kern_return_t ReservePlaybackResources(uint64_t guid, IRM::IRMClient& irmClient,
                                           uint8_t channel, uint32_t bandwidthUnits);
    kern_return_t ReserveCaptureResources(uint64_t guid, IRM::IRMClient& irmClient, uint8_t channel,
                                          uint32_t bandwidthUnits);

    // Do not tear down a DirectAudio binding after a failed stop: an ACTIVE
    // OHCI context may still DMA into that mapping.
    [[nodiscard]] kern_return_t StopAll();
    // The caller owns the consumer and must detach it only after StopReceive()
    // has succeeded (ACTIVE clear). Stream 0 is the master; streams 1+ are
    // secondary hardware contexts.
    void SetReceiveConsumer(uint32_t streamIndex,
                            ASFW::Isoch::IIsochReceiveConsumer* consumer) noexcept;
    void NotifyReceiveTimingLoss() noexcept;
    void NotifyReceiveReplayEstablished() noexcept;
    void NotifyReceiveZtsAnchor(uint64_t generation) noexcept;
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
    kern_return_t ClaimDuplexGuid(uint64_t guid);
    void OnReceiveTimingLossDetected() noexcept;
    void StartDeferredTransmitIfReady() noexcept;

    // Stream 0 (master) capture/playback contexts — own the clock/ZTS/replay
    // role. Their lifecycle and callbacks are unchanged from the single-stream
    // design; secondary streams layer on top without touching them.
    std::unique_ptr<ASFW::Isoch::IsochReceiveContext> isochReceiveContext_;
    std::unique_ptr<ASFW::Isoch::IsochTransmitContext> isochTransmitContext_;

    // Secondary streams [1 .. kMaxStreamsPerDirection). Index i here maps to
    // stream (i + 1); each runs on its own OHCI context (contextIndex == stream).
    std::unique_ptr<ASFW::Isoch::IsochReceiveContext>
        secondaryReceiveContexts_[kMaxStreamsPerDirection - 1];
    std::unique_ptr<ASFW::Isoch::IsochTransmitContext>
        secondaryTransmitContexts_[kMaxStreamsPerDirection - 1];

    // First host input channel each capture stream writes (de-interleave offset);
    // recorded here for the audio-engine pass. Index 0 == master (offset 0).
    uint32_t captureChannelOffset_[kMaxStreamsPerDirection]{0, 0, 0, 0};
    ASFW::Isoch::IIsochReceiveConsumer*
        receiveConsumers_[kMaxStreamsPerDirection]{nullptr, nullptr, nullptr, nullptr};

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
            return map ? reinterpret_cast<void*>(static_cast<uintptr_t>(map->GetAddress()))
                       : nullptr;
        }
    };

    DVRingMapping dvRing_{};
    ASFW::Isoch::Rx::DVCaptureSink dvSink_{};
    bool dvCaptureActive_{false};

    // Per-stream TX shared resources. Index 0 == master; 1.. are secondary
    // playback streams (multi-stream DICE, e.g. Venice F32 = 2×16). Each IT
    // context DMAs its own slab; the audio engine maps each and writes its
    // de-interleaved 16-ch slice into it.
    OSSharedPtr<IOBufferMemoryDescriptor> txPayloadSlab_[kMaxStreamsPerDirection]{};
    OSSharedPtr<IOBufferMemoryDescriptor> txMetadataRing_[kMaxStreamsPerDirection]{};
    OSSharedPtr<IOBufferMemoryDescriptor> txControlBlock_[kMaxStreamsPerDirection]{};

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
