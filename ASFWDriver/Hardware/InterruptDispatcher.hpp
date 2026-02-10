#pragma once

#include <cstdint>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#endif

#include "../Controller/ControllerTypes.hpp"

namespace ASFW {
namespace Async {
class AsyncSubsystem;
}
}

namespace ASFW::Driver {

class ControllerCore;
class HardwareInterface;
class IsochService;
class StatusPublisher;

class InterruptDispatcher {
public:
    InterruptDispatcher() = default;
    ~InterruptDispatcher() = default;

    void HandleSnapshot(const InterruptSnapshot& snap,
                        ControllerCore& controller,
                        HardwareInterface& hardware,
                        IODispatchQueue& workQueue,
                        IsochService& isoch,
                        StatusPublisher& statusPublisher,
                        ASFW::Async::AsyncSubsystem* asyncSubsystem);
};

} // namespace ASFW::Driver
