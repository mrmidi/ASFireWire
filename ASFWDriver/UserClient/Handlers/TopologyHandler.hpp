//
//  TopologyHandler.hpp
//  ASFWDriver
//
//  Handler for topology and Self-ID related UserClient methods
//

#ifndef ASFW_USERCLIENT_TOPOLOGY_HANDLER_HPP
#define ASFW_USERCLIENT_TOPOLOGY_HANDLER_HPP

#include <DriverKit/IOUserClient.h>

// Forward declarations
class ASFWDriver;

namespace ASFW::UserClient {

class TopologyHandler {
public:
    explicit TopologyHandler(ASFWDriver* driver);
    ~TopologyHandler() = default;

    // Disable copy/move
    TopologyHandler(const TopologyHandler&) = delete;
    TopologyHandler& operator=(const TopologyHandler&) = delete;

    // Method 5: Get Self-ID capture with raw quadlets and sequences
    kern_return_t GetSelfIDCapture(IOUserClientMethodArguments* args);

    // Method 6: Get complete topology snapshot with nodes and port states
    kern_return_t GetTopologySnapshot(IOUserClientMethodArguments* args);

private:
    ASFWDriver* driver_;
};

} // namespace ASFW::UserClient

#endif // ASFW_USERCLIENT_TOPOLOGY_HANDLER_HPP
