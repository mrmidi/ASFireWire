#pragma once

#include <cstdint>
#include <memory>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/OSSharedPtr.h>
#endif

#include "IsochReceiveContext.hpp"
#include "Core/ExternalSyncBridge.hpp"
#include "Transmit/IsochTransmitContext.hpp"
#include "../Audio/Model/ASFWAudioDevice.hpp"

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

    ASFW::Audio::Model::StreamMode streamMode{ASFW::Audio::Model::StreamMode::kNonBlocking};

    void* rxQueueBase{nullptr};
    uint64_t rxQueueBytes{0};
    void* txQueueBase{nullptr};
    uint64_t txQueueBytes{0};

    void* zeroCopyBase{nullptr};
    uint64_t zeroCopyBytes{0};
    uint32_t zeroCopyFrames{0};
};

class IsochService {
public:
    IsochService() = default;
    ~IsochService() = default;

    kern_return_t StartReceive(uint8_t channel,
                               HardwareInterface& hardware,
                               void* rxQueueBase,
                               uint64_t rxQueueBytes);

    kern_return_t StopReceive();

    kern_return_t StartTransmit(uint8_t channel,
                                HardwareInterface& hardware,
                                uint8_t sid,
                                uint32_t streamModeRaw,
                                uint32_t pcmChannels,
                                uint32_t am824Slots,
                                void* txQueueBase,
                                uint64_t txQueueBytes,
                                void* zeroCopyBase,
                                uint64_t zeroCopyBytes,
                                uint32_t zeroCopyFrames);

    kern_return_t StopTransmit();

    kern_return_t StartDuplex(const IsochDuplexStartParams& params,
                              HardwareInterface& hardware);

    kern_return_t StopDuplex(uint64_t guid);

    void StopAll();

    ASFW::Isoch::IsochReceiveContext* ReceiveContext() const { return isochReceiveContext_.get(); }
    ASFW::Isoch::IsochTransmitContext* TransmitContext() const { return isochTransmitContext_.get(); }

private:
    ASFW::Isoch::Core::ExternalSyncBridge externalSyncBridge_{};
    OSSharedPtr<ASFW::Isoch::IsochReceiveContext> isochReceiveContext_;
    std::unique_ptr<ASFW::Isoch::IsochTransmitContext> isochTransmitContext_;

    uint64_t activeGuid_{0};
};

} // namespace ASFW::Driver
