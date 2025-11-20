//
//  StatusHandler.hpp
//  ASFWDriver
//
//  Handler for controller status related UserClient methods
//

#ifndef ASFW_USERCLIENT_STATUS_HANDLER_HPP
#define ASFW_USERCLIENT_STATUS_HANDLER_HPP

#include <DriverKit/IOUserClient.h>

// Forward declarations
class ASFWDriver;
class ASFWDriverUserClient;

namespace ASFW::UserClient {

class StatusHandler {
public:
    explicit StatusHandler(ASFWDriver* driver);
    ~StatusHandler() = default;

    // Disable copy/move
    StatusHandler(const StatusHandler&) = delete;
    StatusHandler& operator=(const StatusHandler&) = delete;

    // Method 2: Get comprehensive controller status
    kern_return_t GetControllerStatus(IOUserClientMethodArguments* args);

    // Method 3: Get metrics snapshot (currently unsupported)
    kern_return_t GetMetricsSnapshot(IOUserClientMethodArguments* args);

    // Method 7: Simple health check ping
    kern_return_t Ping(IOUserClientMethodArguments* args);

    // Method 10: Register for status change notifications
    kern_return_t RegisterStatusListener(IOUserClientMethodArguments* args,
                                         ASFWDriverUserClient* userClient);

    // Method 11: Copy controller snapshot via ASFWDriver
    kern_return_t CopyStatusSnapshot(IOUserClientMethodArguments* args);

private:
    ASFWDriver* driver_;
};

} // namespace ASFW::UserClient

#endif // ASFW_USERCLIENT_STATUS_HANDLER_HPP
