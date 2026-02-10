#pragma once

#include <cstdint>
#include <memory>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/OSSharedPtr.h>
#endif

#include "IsochReceiveContext.hpp"
#include "Transmit/IsochTransmitContext.hpp"

namespace ASFW {
namespace Discovery {
class DeviceManager;
}
namespace Protocols {
namespace AVC {
class AVCDiscovery;
}
}
namespace CMP {
class CMPClient;
}
}

namespace ASFW::Driver {

class HardwareInterface;

class IsochService {
public:
    IsochService() = default;
    ~IsochService() = default;

    kern_return_t StartReceive(uint8_t channel,
                               HardwareInterface& hardware,
                               ASFW::Discovery::DeviceManager* deviceManager,
                               ASFW::CMP::CMPClient* cmpClient,
                               ASFW::Protocols::AVC::AVCDiscovery* avcDiscovery = nullptr);

    kern_return_t StopReceive(ASFW::CMP::CMPClient* cmpClient);

    kern_return_t StartTransmit(uint8_t channel,
                                HardwareInterface& hardware,
                                ASFW::Discovery::DeviceManager* deviceManager,
                                ASFW::CMP::CMPClient* cmpClient,
                                ASFW::Protocols::AVC::AVCDiscovery* avcDiscovery);

    kern_return_t StopTransmit(ASFW::CMP::CMPClient* cmpClient);

    void StopAll();

    ASFW::Isoch::IsochReceiveContext* ReceiveContext() const { return isochReceiveContext_.get(); }
    ASFW::Isoch::IsochTransmitContext* TransmitContext() const { return isochTransmitContext_.get(); }

private:
    OSSharedPtr<ASFW::Isoch::IsochReceiveContext> isochReceiveContext_;
    std::unique_ptr<ASFW::Isoch::IsochTransmitContext> isochTransmitContext_;
};

} // namespace ASFW::Driver
