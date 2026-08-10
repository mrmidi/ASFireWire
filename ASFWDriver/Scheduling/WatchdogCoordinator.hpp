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
class IAsyncSubsystemPort;
}
namespace Isoch {
class IsochReceiveContext;
class IsochTransmitContext;
} // namespace Isoch
} // namespace ASFW

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

    void HandleTick(ControllerCore* controller, ASFW::Async::IAsyncSubsystemPort* asyncSubsystem,
                    ASFW::Isoch::IsochReceiveContext* isochReceiveContext,
                    ASFW::Isoch::IsochTransmitContext* isochTransmitContext,
                    StatusPublisher& statusPublisher);

  private:
    void TickAsyncSubsystem(ASFW::Async::IAsyncSubsystemPort* asyncSubsystem,
                            StatusPublisher& statusPublisher) const;
    void TickIsochReceive(ASFW::Isoch::IsochReceiveContext* isochReceiveContext);
    void TickIsochTransmit(ASFW::Isoch::IsochTransmitContext* isochTransmitContext);

    OSSharedPtr<IOTimerDispatchSource> timer_;
    OSSharedPtr<OSAction> action_;
    uint32_t isochLogDivider_{0};
    uint32_t receiveProgressLogDivider_{0};
    uint32_t itLogDivider_{0};
    uint32_t receiveTelemetryDivider_{0};
    uint32_t receiveDiagnosticsDivider_{0};
    uint32_t receiveTraceDivider_{0};
    // Edge-triggered so a permanently ineligible drain reports once, not at
    // 1 kHz. Starts true: the interesting event is the drop to false.
    bool lastDrainEligible_{true};
};

} // namespace ASFW::Driver
