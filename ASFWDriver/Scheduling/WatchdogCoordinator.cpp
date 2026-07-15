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
// ZTS telemetry drain cadence. The watchdog ticks every 1 ms; draining every
// 100 ticks (~100 ms) keeps the ring drained well below its capacity. The
// receive context always logs the seed, then gates steady-state snapshots to
// one per ~4 seconds while retaining the multi-second clock-drift measurement.
constexpr uint32_t kZtsDrainIntervalTicks = 100;
constexpr uint32_t kZtsRecordsPerDrain = 8;
// Audio payload writer decisions arrive ~100/s, so drain every 100 ticks
// (~100 ms) and emit up to 8 strided records.
constexpr uint32_t kPayloadWriterDrainIntervalTicks = 100;
constexpr uint32_t kPayloadWriterRecordsPerDrain = 8;
// TX SYT decisions arrive ~6000/s; the trace is a single latest-value mailbox,
// so log the most recent decision once per ~1 s (1000 ticks). The line carries
// the total decision count so collapsed updates are still visible.
constexpr uint32_t kTxSytTraceIntervalTicks = 1000;

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
    Stop();
    action_.reset();
    timer_.reset();
    isochLogDivider_ = 0;
    itLogDivider_ = 0;
    ztsLogDivider_ = 0;
    payloadWriterLogDivider_ = 0;
    txSytTraceDivider_ = 0;
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

    // ZTS clock telemetry is captured lock-free inside Poll() (which runs in
    // the interrupt hot path); format it here, off the hot path, on a ~100 ms
    // cadence (tick = 1 ms). Gated by the DirectAudio verbosity so it shares
    // the direct-audio diagnostics kill switch (default on). Draining remains
    // frequent; the receive-side log gate controls the much lower print rate.
    if (isRunning && ::ASFW::LogConfig::Shared().GetDirectAudioVerbosity() >= 1) {
        if (++ztsLogDivider_ >= kZtsDrainIntervalTicks) {
            ztsLogDivider_ = 0;
            isochReceiveContext->DrainZtsTelemetry(kZtsRecordsPerDrain);
        }
        if (++payloadWriterLogDivider_ >= kPayloadWriterDrainIntervalTicks) {
            payloadWriterLogDivider_ = 0;
            isochReceiveContext->DrainPayloadWriterTelemetry(kPayloadWriterRecordsPerDrain);
        }
        if (++txSytTraceDivider_ >= kTxSytTraceIntervalTicks) {
            txSytTraceDivider_ = 0;
            isochReceiveContext->LogTxSytTrace();
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
