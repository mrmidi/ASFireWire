#include "WatchdogCoordinator.hpp"

#include <DriverKit/IOLib.h>

#include <net.mrmidi.ASFW.ASFWDriver/ASFWDriver.h>

#include "../Async/Interfaces/IAsyncSubsystemPort.hpp"
#include "../Controller/ControllerCore.hpp"
#include "../Diagnostics/StatusPublisher.hpp"
#include "../Isoch/IsochReceiveContext.hpp"
#include "../Isoch/Transmit/IsochTransmitContext.hpp"
#include "../Logging/LogConfig.hpp"

namespace ASFW::Driver {
namespace {
// Content-neutral maintenance cadence for an installed receive consumer. The
// watchdog selects only bounded work classes; the consumer owns their meaning.
constexpr uint32_t kReceiveTelemetryIntervalTicks = 100;
constexpr uint32_t kReceiveTelemetryRecordsPerDrain = 8;
constexpr uint32_t kReceiveTraceIntervalTicks = 1000;

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
        // NOTE: Do NOT call Cancel(nullptr) here.
        // Cancel() dispatches an async block on the work queue. If the timer
        // is released (via Reset() → timer_.reset()) before that block executes,
        // the block dereferences a freed object → SIGSEGV at 0x10 in Cancel_Impl.
        // Disabling the timer is sufficient; the dispatch source is cleaned up
        // when the shared pointer is released.
    }
}

void WatchdogCoordinator::Reset() {
    // Cancel is terminal. Keep both the source and its OSAction alive until
    // DriverKit confirms that every queued timer callback has returned; the
    // timer retains its handler until cancellation (IOTimerDispatchSource.h).
    if (timer_) {
        IOTimerDispatchSource* timer = timer_.detach();
        OSAction* action = action_.detach();
        const kern_return_t kr = timer->Cancel(^{
            if (action) {
                action->release();
            }
            timer->release();
        });
        if (kr != kIOReturnSuccess) {
            if (action) {
                action->release();
            }
            timer->release();
        }
    } else {
        action_.reset();
    }
    isochLogDivider_ = 0;
    receiveProgressLogDivider_ = 0;
    itLogDivider_ = 0;
    receiveTelemetryDivider_ = 0;
    receiveTraceDivider_ = 0;
    lastDrainEligible_ = true;
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
                                     ASFW::Async::IAsyncSubsystemPort* asyncSubsystem,
                                     ASFW::Isoch::IsochReceiveContext* isochReceiveContext,
                                     ASFW::Isoch::IsochTransmitContext* isochTransmitContext,
                                     StatusPublisher& statusPublisher) {
    TickAsyncSubsystem(asyncSubsystem, statusPublisher);
    TickIsochReceive(isochReceiveContext);
    TickIsochTransmit(isochTransmitContext);
    statusPublisher.Publish(controller, asyncSubsystem, SharedStatusReason::Watchdog);
}

void WatchdogCoordinator::TickAsyncSubsystem(
    ASFW::Async::IAsyncSubsystemPort* asyncSubsystem,
    StatusPublisher& statusPublisher) const {
    if (!asyncSubsystem) {
        return;
    }

    asyncSubsystem->OnTimeoutTick();
    const auto stats = asyncSubsystem->GetWatchdogStats();
    statusPublisher.UpdateAsyncWatchdog(static_cast<uint32_t>(stats.expiredTransactions),
                                        stats.tickCount,
                                        stats.lastTickUsec);
}

void WatchdogCoordinator::TickIsochReceive(
    ASFW::Isoch::IsochReceiveContext* isochReceiveContext) {
    if (!isochReceiveContext) {
        return;
    }

    const bool isRunning =
        isochReceiveContext->GetState() == ASFW::Isoch::IRPolicy::State::Running;
    if (isRunning) {
        isochReceiveContext->Poll();
    }

    // Run bounded consumer maintenance off the receive polling hot path. The
    // transport/scheduler never interprets consumer telemetry or trace data.
    const bool drainEligible =
        isRunning && ::ASFW::LogConfig::Shared().GetDirectAudioVerbosity() >= 1;
    if (drainEligible != lastDrainEligible_) {
        lastDrainEligible_ = drainEligible;
        ASFW_LOG(Isoch, "[RxDrain] eligible=%d running=%d verbosity=%u", drainEligible,
                 isRunning, ::ASFW::LogConfig::Shared().GetDirectAudioVerbosity());
    }

    if (drainEligible) {
        if (++receiveTelemetryDivider_ >= kReceiveTelemetryIntervalTicks) {
            receiveTelemetryDivider_ = 0;
            isochReceiveContext->RunConsumerMaintenance(
                ASFW::Isoch::IsochConsumerMaintenanceKind::kTelemetryDrain,
                kReceiveTelemetryRecordsPerDrain);
        }
        if (++receiveTraceDivider_ >= kReceiveTraceIntervalTicks) {
            receiveTraceDivider_ = 0;
            isochReceiveContext->RunConsumerMaintenance(
                ASFW::Isoch::IsochConsumerMaintenanceKind::kTraceSnapshot,
                1);
        }
    }

    if (++receiveProgressLogDivider_ >= 1000) {
        receiveProgressLogDivider_ = 0;
        if (isRunning) {
            isochReceiveContext->LogProgressStatistics();
        }
    }

    if (++isochLogDivider_ < 500) {
        return;
    }
    isochLogDivider_ = 0;
    if (isRunning && (::ASFW::LogConfig::Shared().GetIsochVerbosity() >= 3)) {
        isochReceiveContext->LogHardwareState();
    }
}

void WatchdogCoordinator::TickIsochTransmit(
    ASFW::Isoch::IsochTransmitContext* isochTransmitContext) {
    if (!isochTransmitContext) {
        return;
    }

    const bool isRunning =
        isochTransmitContext->GetState() == ASFW::Isoch::ITState::Running;
    if (isRunning) {
        isochTransmitContext->Poll();
    }

    if (++itLogDivider_ < 1000) {
        return;
    }
    itLogDivider_ = 0;
    if (isRunning) {
        isochTransmitContext->LogStatistics();
    }
}

} // namespace ASFW::Driver
