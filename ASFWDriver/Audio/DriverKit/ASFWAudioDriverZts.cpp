//
// ASFWAudioDriverZts.cpp
// ASFWDriver
//
// Zero-timestamp mirror pump for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../../AudioWire/AMDTP/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>

#include <cmath>
#include <cstring>

namespace ASFW::Audio::DriverKit {
namespace {

[[nodiscard]] int64_t NanosecondsToMachTicksSigned(int64_t nanos) noexcept {
    const uint64_t ticks = ASFW::Timing::nanosToHostTicks(static_cast<uint64_t>(std::abs(nanos)));
    return (nanos < 0) ? -static_cast<int64_t>(ticks) : static_cast<int64_t>(ticks);
}

[[nodiscard]] uint64_t MicrosecondsToMachTicks(uint64_t usec) noexcept {
    static mach_timebase_info_data_t timebase{0, 0};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    if (timebase.numer == 0) {
        return 0;
    }

    const __uint128_t nanos = static_cast<__uint128_t>(usec) * 1000u;
    const __uint128_t scaled = nanos * timebase.denom;
    return static_cast<uint64_t>(scaled / timebase.numer);
}

} // namespace

ASFW::Audio::Runtime::ZtsMirrorPublishResult PublishSharedZeroTimestampToHAL(ASFWAudioDriver_IVars& ivars,
                                                                             const char* reason,
                                                                             bool logSuccess) noexcept {
    auto* control = ivars.runtime.directAudioGraph.control;
    auto* audioDevice = ivars.audioDevice.get();
    if (!control || !audioDevice) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::NotReady;
    }

    const uint64_t generation = control->ztsState.sourceGeneration.load(std::memory_order_acquire);
    if (generation == 0) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::NotReady;
    }
    if (generation == ivars.runtime.lastHalZeroTimestampGeneration.load(std::memory_order_acquire)) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::AlreadyPublished;
    }

    const auto source = control->ztsState.selectedSource.load(std::memory_order_relaxed);
    if (source == ASFW::Audio::Runtime::ZtsAuthoritySource::None) {
        ASFW_LOG(DirectAudio, "ADK FATAL ZTS mirror pump failed: selectedSource is None!");
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::InvalidAuthority;
    }

    const uint64_t eventSampleFrame = control->ztsState.authoritativeSampleFrame.load(std::memory_order_relaxed);
    const uint64_t eventHostTicks = control->ztsState.authoritativeHostTicks.load(std::memory_order_relaxed);
    const uint32_t eventHostNanosPerSampleQ8 = control->ztsState.hostNanosPerSampleQ8.load(std::memory_order_relaxed);
    if (eventHostTicks == 0 || eventHostNanosPerSampleQ8 == 0) {
        ASFW_LOG(DirectAudio, "ADK FATAL ZTS mirror pump failed: invalid ticks (%llu) or nanosPerSample (%u) for source %{public}s",
                 eventHostTicks, eventHostNanosPerSampleQ8, ToString(source));
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::InvalidTimeline;
    }

    ivars.runtime.lastHalZeroTimestampGeneration.store(generation, std::memory_order_release);

    const uint32_t P = ASFW::Isoch::Config::kAudioIoPeriodFrames;
    if (P == 0) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::InvalidTimeline;
    }

    uint64_t nextFrame = ivars.runtime.nextExpectedZtsFrame.load(std::memory_order_acquire);
    if (!ivars.runtime.ztsTimelineInitialized.load(std::memory_order_acquire)) {
        const uint64_t alignedStart = (eventSampleFrame / P) * P;
        ivars.runtime.nextExpectedZtsFrame.store(alignedStart, std::memory_order_release);
        ivars.runtime.ztsTimelineInitialized.store(true, std::memory_order_release);
        nextFrame = alignedStart;
    }

    bool publishedAny = false;
    while (eventSampleFrame >= nextFrame) {
        if (ivars.runtime.nextExpectedZtsFrame.compare_exchange_strong(nextFrame, nextFrame + P, std::memory_order_acq_rel)) {
            const int64_t deltaSamples = static_cast<int64_t>(nextFrame) - static_cast<int64_t>(eventSampleFrame);
            const int64_t deltaNanos = (deltaSamples * static_cast<int64_t>(eventHostNanosPerSampleQ8)) >> 8;
            const int64_t deltaTicks = NanosecondsToMachTicksSigned(deltaNanos);
            const uint64_t targetHostTicks = eventHostTicks + deltaTicks;

            audioDevice->UpdateCurrentZeroTimestamp(nextFrame, targetHostTicks);
            publishedAny = true;

            if (logSuccess || (reason && std::strcmp(reason, "prime") == 0)) {
                const uint64_t rxZts = control->counters.ztsRxPublished.load(std::memory_order_relaxed);
                const uint64_t rxAdk = control->counters.ztsRxAdkPublished.load(std::memory_order_relaxed);
                ASFW_LOG(DirectAudio,
                         "ADK DBG ZTS publish reason=%{public}s sample=%llu deltaSample=%lld deltaHost=%lld period=%u source=%{public}s rxZts=%llu rxAdk=%llu",
                         reason ? reason : "unknown",
                         nextFrame,
                         deltaSamples,
                         deltaTicks,
                         P,
                         ToString(source),
                         rxZts,
                         rxAdk);
            }

            control->ztsState.mirrorPublications.fetch_add(1, std::memory_order_relaxed);
            control->counters.CountRxAdkZtsPublished();
            nextFrame += P;
        } else {
            nextFrame = ivars.runtime.nextExpectedZtsFrame.load(std::memory_order_acquire);
        }
    }

    return publishedAny ? ASFW::Audio::Runtime::ZtsMirrorPublishResult::Published 
                        : ASFW::Audio::Runtime::ZtsMirrorPublishResult::NoNewGeneration;
}

bool PrimeSharedZeroTimestampToHAL(ASFWAudioDriver_IVars& ivars) noexcept {
    constexpr uint32_t kAttempts = 100;
    constexpr uint32_t kDelayUsec = 1000;
    for (uint32_t attempt = 0; attempt < kAttempts; ++attempt) {
        if (PublishSharedZeroTimestampToHAL(ivars, "prime", true) == ASFW::Audio::Runtime::ZtsMirrorPublishResult::Published) {
            return true;
        }
        IODelay(kDelayUsec);
    }
    return false;
}

void ScheduleZtsMirrorTimer(ASFWAudioDriver_IVars& ivars) noexcept {
    if (!ivars.ztsMirrorTimer ||
        !ivars.runtime.isRunning.load(std::memory_order_acquire)) {
        return;
    }

    const uint64_t delta = MicrosecondsToMachTicks(kZtsMirrorPumpPeriodUsec);
    if (delta == 0) {
        return;
    }
    (void)ivars.ztsMirrorTimer->WakeAtTime(kIOTimerClockMachAbsoluteTime,
                                           mach_absolute_time() + delta,
                                           0);
}

void StopZtsMirrorTimer(ASFWAudioDriver_IVars& ivars) noexcept {
    if (ivars.ztsMirrorTimer) {
        (void)ivars.ztsMirrorTimer->SetEnableWithCompletion(false, nullptr);
    }
}

bool EnsureZtsMirrorTimer(ASFWAudioDriver& driver,
                          ASFWAudioDriver_IVars& ivars) noexcept {
    if (ivars.ztsMirrorTimer && ivars.ztsMirrorAction) {
        const kern_return_t kr = ivars.ztsMirrorTimer->SetEnableWithCompletion(true, nullptr);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(DirectAudio, "ADK WARN ZTS mirror reenable failed kr=0x%x", kr);
            return false;
        }
        return true;
    }
    if (!ivars.workQueue) {
        ASFW_LOG(DirectAudio, "ADK WARN ZTS mirror missing_work_queue");
        return false;
    }

    IOTimerDispatchSource* timer = nullptr;
    kern_return_t kr = IOTimerDispatchSource::Create(ivars.workQueue.get(), &timer);
    if (kr != kIOReturnSuccess || !timer) {
        ASFW_LOG(DirectAudio, "ADK WARN ZTS mirror timer_create failed kr=0x%x", kr);
        return false;
    }
    ivars.ztsMirrorTimer = OSSharedPtr(timer, OSNoRetain);

    OSAction* action = nullptr;
    kr = driver.CreateActionZtsMirrorTimerFired(0, &action);
    if (kr != kIOReturnSuccess || !action) {
        ASFW_LOG(DirectAudio, "ADK WARN ZTS mirror action_create failed kr=0x%x", kr);
        ivars.ztsMirrorTimer.reset();
        return false;
    }
    ivars.ztsMirrorAction = OSSharedPtr(action, OSNoRetain);

    kr = ivars.ztsMirrorTimer->SetHandler(ivars.ztsMirrorAction.get());
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK WARN ZTS mirror set_handler failed kr=0x%x", kr);
        ivars.ztsMirrorAction.reset();
        ivars.ztsMirrorTimer.reset();
        return false;
    }

    kr = ivars.ztsMirrorTimer->SetEnableWithCompletion(true, nullptr);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(DirectAudio, "ADK WARN ZTS mirror enable failed kr=0x%x", kr);
        ivars.ztsMirrorAction.reset();
        ivars.ztsMirrorTimer.reset();
        return false;
    }
    return true;
}

} // namespace ASFW::Audio::DriverKit

void ASFWAudioDriver::ZtsMirrorTimerFired_Impl(ASFWAudioDriver_ZtsMirrorTimerFired_Args)
{
    if (!ivars || !ivars->runtime.isRunning.load(std::memory_order_acquire)) {
        return;
    }

    const auto* control = ivars->runtime.directAudioGraph.control;
    if (control) {
        const auto fatal = control->fatalReason.load(std::memory_order_acquire);
        if (fatal != ASFW::Audio::Runtime::FatalStreamReason::None) {
            ASFW_LOG(DirectAudio, "ADK FATAL: teardown requested by isoch path, reason=%u", static_cast<uint32_t>(fatal));
            ASFW::Audio::DriverKit::PerformLoudTeardown(*ivars, "Isoch path fatal error");
            return;
        }
    }

    const auto result = ASFW::Audio::DriverKit::PublishSharedZeroTimestampToHAL(*ivars, "pump", false);
    const bool published = (result == ASFW::Audio::Runtime::ZtsMirrorPublishResult::Published);

    bool shouldTeardown = false;
    if (result == ASFW::Audio::Runtime::ZtsMirrorPublishResult::InvalidAuthority ||
        result == ASFW::Audio::Runtime::ZtsMirrorPublishResult::InvalidTimeline) {
        shouldTeardown = true;
    } else if (result == ASFW::Audio::Runtime::ZtsMirrorPublishResult::NotReady &&
               ivars->runtime.ztsTimelineInitialized.load(std::memory_order_acquire)) {
        shouldTeardown = true;
    }

    if (shouldTeardown) {
        ASFW::Audio::DriverKit::PerformLoudTeardown(*ivars, "ZTS mirror pump failed/authority lost");
        return;
    }

    const uint64_t tick = ivars->ztsMirrorTimerTicks.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t beginReadCount =
        control ? control->counters.ioBeginReadCount.load(std::memory_order_relaxed) : 0;
    const uint64_t writeEndCount =
        control ? control->counters.ioWriteEndCount.load(std::memory_order_relaxed) : 0;

    if (published && (beginReadCount == 0 || writeEndCount == 0) &&
        (tick <= 8 || (tick % 1024) == 0)) {
        ASFW_LOG(DirectAudio,
                 "ADK DBG ZTS mirror pump tick=%llu guid=0x%016llx beginRead=%llu writeEnd=%llu rxZts=%llu rxAdk=%llu",
                 tick,
                 ivars->device.guid,
                 beginReadCount,
                 writeEndCount,
                 control ? control->counters.ztsRxPublished.load(std::memory_order_relaxed) : 0,
                 control ? control->counters.ztsRxAdkPublished.load(std::memory_order_relaxed) : 0);
    }

    ASFW::Audio::DriverKit::ScheduleZtsMirrorTimer(*ivars);
}
