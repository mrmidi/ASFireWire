//
//  IsochHandler.hpp
//  ASFWDriver
//
//  Handler for Isochronous Operations (IRM, CMP, Streaming)
//

#pragma once

#include <cstdint>

#include <DriverKit/IOReturn.h>

// Forward declarations
struct IOUserClientMethodArguments;
class ASFWDriver;

namespace ASFW::UserClient {

class IsochHandler {
public:
    IsochHandler(::ASFWDriver* driver, uint64_t ownerToken);
    ~IsochHandler() = default;

    void ReleaseOwner() noexcept;

    // IRM Test Methods
    kern_return_t TestIRMAllocation(IOUserClientMethodArguments* args);
    kern_return_t TestIRMRelease(IOUserClientMethodArguments* args);

    // CMP Test Methods
    kern_return_t TestCMPConnectOPCR(IOUserClientMethodArguments* args);
    kern_return_t TestCMPDisconnectOPCR(IOUserClientMethodArguments* args);
    kern_return_t TestCMPConnectIPCR(IOUserClientMethodArguments* args);
    kern_return_t TestCMPDisconnectIPCR(IOUserClientMethodArguments* args);

    // Isoch Streaming Control
    kern_return_t StartIsochReceive(IOUserClientMethodArguments* args);
    kern_return_t StopIsochReceive(IOUserClientMethodArguments* args);

    // DV Capture Control
    kern_return_t StartDVCapture(IOUserClientMethodArguments* args);
    kern_return_t StopDVCapture(IOUserClientMethodArguments* args);
    
    // Isoch Metrics
    kern_return_t GetIsochRxMetrics(IOUserClientMethodArguments* args);
    kern_return_t ResetIsochRxMetrics(IOUserClientMethodArguments* args);
    
    // IT Streaming Control (DMA allocation only - no CMP)
    kern_return_t StartIsochTransmit(IOUserClientMethodArguments* args);
    kern_return_t StopIsochTransmit(IOUserClientMethodArguments* args);

private:
    ::ASFWDriver* driver_;
    uint64_t ownerToken_{0};
    bool ownsDVCapture_{false};
};

} // namespace ASFW::UserClient
