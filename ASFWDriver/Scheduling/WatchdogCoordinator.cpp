#include "WatchdogCoordinator.hpp"

#include <DriverKit/IOLib.h>

#include <net.mrmidi.ASFW.ASFWDriver/ASFWDriver.h>

#include "../Async/AsyncSubsystem.hpp"
#include "../Controller/ControllerCore.hpp"
#include "../Diagnostics/StatusPublisher.hpp"
#include "../Isoch/IsochReceiveContext.hpp"
#include "../Isoch/Transmit/IsochTransmitContext.hpp"

namespace ASFW::Driver {
namespace {
uint64_t MicrosecondsToMachTicks(uint64_t usec) {
    static mach_timebase_info_data_t timebase{0, 0};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }

    const __uint128_t nanos = static_cast<__uint128_t>(usec) * 1000u;
    const __uint128_t scaled = nanos * timebase.denom;
    return static_cast<uint64_t>(scaled / timebase.numer);
}
} // namespace

kern_return_t WatchdogCoordinator::Prepare(::ASFWDriver& service,
                                           OSSharedPtr<IODispatchQueue> workQueue) {
    if (!workQueue) {
        return kIOReturnNotReady;
    }

    IOTimerDispatchSource* timer = nullptr;
    auto kr = IOTimerDispatchSource::Create(workQueue.get(), &timer);
    if (kr != kIOReturnSuccess || !timer) {
        return kr != kIOReturnSuccess ? kr : kIOReturnNoResources;
    }
    timer_ = OSSharedPtr(timer, OSNoRetain);

    OSAction* action = nullptr;
    kr = service.CreateActionAsyncWatchdogTimerFired(0, &action);
    if (kr != kIOReturnSuccess || !action) {
        timer_.reset();
        return kr != kIOReturnSuccess ? kr : kIOReturnError;
    }
    action_ = OSSharedPtr(action, OSNoRetain);

    kr = timer_->SetHandler(action_.get());
    if (kr != kIOReturnSuccess) {
        action_.reset();
        timer_.reset();
        return kr;
    }

    kr = timer_->SetEnableWithCompletion(true, nullptr);
    if (kr != kIOReturnSuccess) {
        action_.reset();
        timer_.reset();
        return kr;
    }

    return kIOReturnSuccess;
}

void WatchdogCoordinator::Stop() {
    if (timer_) {
        timer_->SetEnableWithCompletion(false, nullptr);
        timer_->Cancel(nullptr);
    }
}

void WatchdogCoordinator::Reset() {
    Stop();
    action_.reset();
    timer_.reset();
    isochLogDivider_ = 0;
    itLogDivider_ = 0;
}

void WatchdogCoordinator::Schedule(uint64_t delayUsec) {
    if (!timer_) {
        return;
    }

    const uint64_t now = mach_absolute_time();
    const uint64_t delta = MicrosecondsToMachTicks(delayUsec);
    (void)timer_->WakeAtTime(kIOTimerClockMachAbsoluteTime, now + delta, 0);
}

void WatchdogCoordinator::HandleTick(ControllerCore* controller,
                                     ASFW::Async::AsyncSubsystem* asyncSubsystem,
                                     ASFW::Isoch::IsochReceiveContext* isochReceiveContext,
                                     ASFW::Isoch::IsochTransmitContext* isochTransmitContext,
                                     StatusPublisher& statusPublisher) {
    if (asyncSubsystem) {
        asyncSubsystem->OnTimeoutTick();
        const auto stats = asyncSubsystem->GetWatchdogStats();
        statusPublisher.UpdateAsyncWatchdog(static_cast<uint32_t>(stats.expiredTransactions),
                                            stats.tickCount,
                                            stats.lastTickUsec);
    }

    if (isochReceiveContext) {
        if (isochReceiveContext->GetState() == ASFW::Isoch::IRPolicy::State::Running) {
            isochReceiveContext->Poll();
        }
        if (++isochLogDivider_ >= 500) {
            isochLogDivider_ = 0;
            if (isochReceiveContext->GetState() == ASFW::Isoch::IRPolicy::State::Running) {
                isochReceiveContext->GetStreamProcessor().LogStatistics();
                isochReceiveContext->LogHardwareState();
            }
        }
    }

    if (isochTransmitContext) {
        if (isochTransmitContext->GetState() == ASFW::Isoch::ITState::Running) {
            isochTransmitContext->Poll();
        }
        if (++itLogDivider_ >= 1000) {
            itLogDivider_ = 0;
            if (isochTransmitContext->GetState() == ASFW::Isoch::ITState::Running) {
                isochTransmitContext->LogStatistics();
            }
        }
    }

    statusPublisher.Publish(controller, asyncSubsystem, SharedStatusReason::Watchdog);
}

} // namespace ASFW::Driver
