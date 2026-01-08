//
//  IsochHandler.hpp
//  ASFWDriver
//
//  Handler for Isochronous Operations (IRM, CMP, Streaming)
//

#pragma once

#include <DriverKit/IOReturn.h>

// Forward declarations
struct IOUserClientMethodArguments;
class ASFWDriver;

namespace ASFW::UserClient {

class IsochHandler {
public:
    explicit IsochHandler(::ASFWDriver* driver);
    ~IsochHandler() = default;

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
    
    // Isoch Metrics
    kern_return_t GetIsochRxMetrics(IOUserClientMethodArguments* args);
    kern_return_t ResetIsochRxMetrics(IOUserClientMethodArguments* args);
    
    // IT Streaming Control (DMA allocation only - no CMP)
    kern_return_t StartIsochTransmit(IOUserClientMethodArguments* args);
    kern_return_t StopIsochTransmit(IOUserClientMethodArguments* args);

private:
    ::ASFWDriver* driver_;
};

} // namespace ASFW::UserClient
