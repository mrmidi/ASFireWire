#pragma once

#include <cstdint>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSSharedPtr.h>
#endif

namespace ASFW {
namespace Async {
class AsyncSubsystem;
}
namespace Isoch {
class IsochReceiveContext;
class IsochTransmitContext;
}
}

class ASFWDriver;

namespace ASFW::Driver {
class ControllerCore;
class StatusPublisher;

class WatchdogCoordinator {
public:
    WatchdogCoordinator() = default;
    ~WatchdogCoordinator() = default;

    kern_return_t Prepare(::ASFWDriver& service, OSSharedPtr<IODispatchQueue> workQueue);
    void Stop();
    void Reset();

    void Schedule(uint64_t delayUsec);

    void HandleTick(ControllerCore* controller,
                    ASFW::Async::AsyncSubsystem* asyncSubsystem,
                    ASFW::Isoch::IsochReceiveContext* isochReceiveContext,
                    ASFW::Isoch::IsochTransmitContext* isochTransmitContext,
                    StatusPublisher& statusPublisher);

private:
    OSSharedPtr<IOTimerDispatchSource> timer_;
    OSSharedPtr<OSAction> action_;
    uint32_t isochLogDivider_{0};
    uint32_t itLogDivider_{0};
};

} // namespace ASFW::Driver
