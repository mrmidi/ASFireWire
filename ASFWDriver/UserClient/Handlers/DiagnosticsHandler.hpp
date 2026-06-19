#ifndef ASFW_USERCLIENT_DIAGNOSTICS_HANDLER_HPP
#define ASFW_USERCLIENT_DIAGNOSTICS_HANDLER_HPP

#include <DriverKit/IOUserClient.h>

class ASFWDriver;

namespace ASFW::Diagnostics {
class DiagnosticsService;
}

namespace ASFW::UserClient {

class DiagnosticsHandler {
public:
    explicit DiagnosticsHandler(ASFWDriver* driver) noexcept;
    ~DiagnosticsHandler();

    // Disable copy/move
    DiagnosticsHandler(const DiagnosticsHandler&) = delete;
    DiagnosticsHandler& operator=(const DiagnosticsHandler&) = delete;

    // Selector 1000: Bus Contract
    kern_return_t GetBusContract(IOUserClientMethodArguments* args);
    
    // Selector 1001: Topology
    kern_return_t GetTopology(IOUserClientMethodArguments* args);
    
    // Selector 1002: Role Coordinator
    kern_return_t GetRoleCoordinator(IOUserClientMethodArguments* args);
    
    // Selector 1003: OHCI Registers
    kern_return_t GetOHCI(IOUserClientMethodArguments* args);
    
    // Selector 1004: PHY Registers
    kern_return_t GetPHY(IOUserClientMethodArguments* args);
    
    // Selector 1005: CSR Contract
    kern_return_t GetCSRContract(IOUserClientMethodArguments* args);
    
    // Selector 1006: Async Trace
    kern_return_t GetAsyncTrace(IOUserClientMethodArguments* args);
    
    // Selector 1007: Inbound CSR Stats
    kern_return_t GetInboundCSRStats(IOUserClientMethodArguments* args);
    
    // Selector 1008: Clear Async Trace
    kern_return_t ClearAsyncTrace(IOUserClientMethodArguments* args);

    // Selector 1009: Bus Manager Info
    kern_return_t GetBusManager(IOUserClientMethodArguments* args);

    // Selector 1010: Post-Reset Timing (IEEE 1394-2008 §8.x) gate states
    kern_return_t GetPostResetTiming(IOUserClientMethodArguments* args);

private:
    ASFWDriver* driver_{nullptr};
    Diagnostics::DiagnosticsService* service_{nullptr};
};

} // namespace ASFW::UserClient

#endif // ASFW_USERCLIENT_DIAGNOSTICS_HANDLER_HPP
