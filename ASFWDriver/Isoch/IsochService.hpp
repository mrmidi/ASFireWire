#pragma once

#include <cstdint>
#include <memory>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include "IsochReceiveContext.hpp"
#include "Core/ExternalSyncBridge.hpp"
#include "Transmit/IsochTransmitContext.hpp"
#include "../Audio/Model/ASFWAudioDevice.hpp"
#include "../Common/DriverKitOwnership.hpp"

namespace ASFW::Driver {

class HardwareInterface;

struct IsochDuplexStartParams {
    uint64_t guid{0};

    uint8_t irChannel{0};  // device -> host
    uint8_t itChannel{0};  // host -> device
    uint8_t sid{0};        // local node number (6-bit)

    uint32_t sampleRateHz{0};

    uint32_t hostInputPcmChannels{0};
    uint32_t hostOutputPcmChannels{0};

    uint32_t deviceToHostAm824Slots{0};
    uint32_t hostToDeviceAm824Slots{0};
    ASFW::Encoding::AudioWireFormat hostToDeviceWireFormat{ASFW::Encoding::AudioWireFormat::kAM824};

    ASFW::Audio::Model::StreamMode streamMode{ASFW::Audio::Model::StreamMode::kNonBlocking};

    OSSharedPtr<IOBufferMemoryDescriptor> rxQueueMemory{};
    uint64_t rxQueueBytes{0};
    OSSharedPtr<IOBufferMemoryDescriptor> txQueueMemory{};
    uint64_t txQueueBytes{0};

    const int32_t* zeroCopyBase{nullptr};
    uint64_t zeroCopyBytes{0};
    uint32_t zeroCopyFrames{0};
};

class IsochService {
public:
    IsochService() = default;
    ~IsochService() = default;

    kern_return_t StartReceive(uint8_t channel,
                               HardwareInterface& hardware,
                               OSSharedPtr<IOBufferMemoryDescriptor> rxQueueMemory,
                               uint64_t rxQueueBytes);

    kern_return_t StopReceive();

    kern_return_t StartTransmit(uint8_t channel,
                                HardwareInterface& hardware,
                                uint8_t sid,
                                uint32_t streamModeRaw,
                                uint32_t pcmChannels,
                                uint32_t am824Slots,
                                ASFW::Encoding::AudioWireFormat wireFormat,
                                OSSharedPtr<IOBufferMemoryDescriptor> txQueueMemory,
                                uint64_t txQueueBytes,
                                const int32_t* zeroCopyBase,
                                uint64_t zeroCopyBytes,
                                uint32_t zeroCopyFrames);

    kern_return_t StopTransmit();

    kern_return_t BeginSplitDuplex(uint64_t guid);

    kern_return_t StartDuplex(const IsochDuplexStartParams& params,
                              HardwareInterface& hardware);

    kern_return_t StopDuplex(uint64_t guid);

    void StopAll();

    ASFW::Isoch::IsochReceiveContext* ReceiveContext() const { return isochReceiveContext_.get(); }
    ASFW::Isoch::IsochTransmitContext* TransmitContext() const { return isochTransmitContext_.get(); }

private:
    kern_return_t ClaimDuplexGuid(uint64_t guid);

    struct SharedQueueMapping {
        OSSharedPtr<IOBufferMemoryDescriptor> memory{};
        OSSharedPtr<IOMemoryMap> map{};
        uint64_t bytes{0};

        void Reset() noexcept {
            map.reset();
            memory.reset();
            bytes = 0;
        }

        [[nodiscard]] uint8_t* BaseAddress() const noexcept {
            return map ? reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(map->GetAddress())) : nullptr;
        }
    };

    ASFW::Isoch::Core::ExternalSyncBridge externalSyncBridge_{};
    OSSharedPtr<ASFW::Isoch::IsochReceiveContext> isochReceiveContext_;
    std::unique_ptr<ASFW::Isoch::IsochTransmitContext> isochTransmitContext_;

    SharedQueueMapping rxQueue_{};
    SharedQueueMapping txQueue_{};

    uint64_t activeGuid_{0};
};

} // namespace ASFW::Driver
