//
//  BusResetHandler.hpp
//  ASFWDriver
//
//  Handler for bus reset related UserClient methods
//

#ifndef ASFW_USERCLIENT_BUS_RESET_HANDLER_HPP
#define ASFW_USERCLIENT_BUS_RESET_HANDLER_HPP

#include <DriverKit/IOUserClient.h>

// Forward declarations
class ASFWDriver;

namespace ASFW::UserClient {

class BusResetHandler {
public:
    explicit BusResetHandler(ASFWDriver* driver);
    ~BusResetHandler() = default;

    // Disable copy/move
    BusResetHandler(const BusResetHandler&) = delete;
    BusResetHandler& operator=(const BusResetHandler&) = delete;

    // Method 0: Get bus reset count, generation, and timestamp
    kern_return_t GetBusResetCount(IOUserClientMethodArguments* args);

    // Method 1: Get bus reset history (array of BusResetPacketWire)
    kern_return_t GetBusResetHistory(IOUserClientMethodArguments* args);

    // Method 4: Clear bus reset packet history
    kern_return_t ClearHistory(IOUserClientMethodArguments* args);

private:
    ASFWDriver* driver_;
};

} // namespace ASFW::UserClient

#endif // ASFW_USERCLIENT_BUS_RESET_HANDLER_HPP
