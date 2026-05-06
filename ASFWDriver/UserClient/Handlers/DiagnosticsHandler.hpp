//
//  DiagnosticsHandler.hpp
//  ASFWDriver
//
//  Handler for bus state diagnostics UserClient methods
//

#ifndef ASFW_USERCLIENT_DIAGNOSTICS_HANDLER_HPP
#define ASFW_USERCLIENT_DIAGNOSTICS_HANDLER_HPP

#include <DriverKit/IOUserClient.h>

// Forward declarations
class ASFWDriver;

namespace ASFW::UserClient {

class DiagnosticsHandler {
public:
    explicit DiagnosticsHandler(ASFWDriver* driver);
    ~DiagnosticsHandler() = default;

    // Disable copy/move
    DiagnosticsHandler(const DiagnosticsHandler&) = delete;
    DiagnosticsHandler& operator=(const DiagnosticsHandler&) = delete;

    // Method 50: Get bus state diagnostics (OHCI registers, topology, PHY, FSM)
    kern_return_t GetBusStateDiagnostics(IOUserClientMethodArguments* args);

    // Method 51: Read PHY register
    kern_return_t ReadPhyRegister(IOUserClientMethodArguments* args);

    // Method 52: Initiate software bus reset
    kern_return_t InitiateBusReset(IOUserClientMethodArguments* args);

private:
    ASFWDriver* driver_;
};

} // namespace ASFW::UserClient

#endif // ASFW_USERCLIENT_DIAGNOSTICS_HANDLER_HPP
