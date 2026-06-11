//
// ASFWAudioDriverZts.cpp
// ASFWDriver
//
// Zero-timestamp mirror pump for ASFWAudioDriver.
//

#include "ASFWAudioDriverPrivate.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"

#include "../Runtime/ZtsTimelineCalculator.hpp"
#include "../Config/TimingCursorPolicy.hpp"

#include <DriverKit/DriverKit.h>
#include <DriverKit/IOLib.h>

#include <cmath>
#include <cstring>

namespace ASFW::Audio::DriverKit {
namespace {

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

    const auto policy = ASFW::Audio::TimingCursorPolicy::MakeDice48kBlocking();
    const uint32_t P = policy.HalZeroTimestampPeriodFrames();
    if (P == 0) {
        return ASFW::Audio::Runtime::ZtsMirrorPublishResult::InvalidTimeline;
    }

    uint64_t nextFrame = ivars.runtime.nextExpectedZtsFrame.load(std::memory_order_acquire);
    if (!ivars.runtime.ztsTimelineInitialized.load(std::memory_order_acquire)) {
        const uint64_t alignedStart = ASFW::Audio::Runtime::ZtsTimelineCalculator::AlignZtsStart(eventSampleFrame, P);
        ivars.runtime.nextExpectedZtsFrame.store(alignedStart, std::memory_order_release);
        ivars.runtime.ztsTimelineInitialized.store(true, std::memory_order_release);
        nextFrame = alignedStart;
    }

    bool publishedAny = false;
    while (eventSampleFrame >= nextFrame) {
        if (ivars.runtime.nextExpectedZtsFrame.compare_exchange_strong(nextFrame, nextFrame + P, std::memory_order_acq_rel)) {
            const int64_t deltaSamples = static_cast<int64_t>(nextFrame) - static_cast<int64_t>(eventSampleFrame);
            const uint64_t targetHostTicks = ASFW::Audio::Runtime::ZtsTimelineCalculator::CalculateTargetHostTicks(
                nextFrame, eventSampleFrame, eventHostTicks, eventHostNanosPerSampleQ8
            );
            const int64_t deltaTicks = static_cast<int64_t>(targetHostTicks) - static_cast<int64_t>(eventHostTicks);

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
    // Isoch is already running here. Allow RX SYT/ZTS establishment to happen
    // asynchronously without making it a prerequisite for starting IR or IT.
    constexpr uint32_t kAttempts = 1000;
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

uint32_t PrepareTransmitSlots(ASFWAudioDriver_IVars& ivars,
                             uint64_t startPacketIndex,
                             uint64_t targetPacketIndex,
                             uint32_t maxToPrepare) noexcept {
    const uint32_t numSlots = ivars.runtime.txSlotProvider.numSlots;
    auto* metadataRing = ivars.runtime.txSlotProvider.metadataRing;
    auto* directControl = ivars.runtime.directAudioGraph.control;
    if (numSlots == 0 || metadataRing == nullptr || directControl == nullptr) {
        return 0;
    }

    uint64_t nextPacketToPrepare = startPacketIndex;
    uint32_t preparedCount = 0;

    while (nextPacketToPrepare < targetPacketIndex && preparedCount < maxToPrepare) {
        int64_t packetAnchorTicks = 0;
        const bool havePacketAnchor =
            ivars.runtime.txExecutionTimeline.AnchorForPacket(
                nextPacketToPrepare, packetAnchorTicks);
        const auto decision = havePacketAnchor
            ? ivars.runtime.txTimingModel.PeekNextDataSyt(
                  packetAnchorTicks,
                  directControl->rxSytCadence)
            : ASFW::Driver::TxTimingModel::Decision{};

        ASFW::Protocols::Audio::AMDTP::AmdtpTimingState timing{};
        timing.txClockValid = decision.syt != 0xFFFFu;
        timing.nextDataSyt = decision.syt;

        if (!ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                static_cast<uint32_t>(nextPacketToPrepare), timing)) {
            break;
        }

        const uint32_t slotIdx = nextPacketToPrepare % numSlots;
        auto& meta = metadataRing[slotIdx];
        if (meta.payloadLength > 8) {
            ivars.runtime.txTimingModel.CommitDataPacket();
        }

        nextPacketToPrepare++;
        preparedCount++;
    }

    return preparedCount;
}

void PrefillTxRingBeforeStart(ASFWAudioDriver_IVars& ivars) noexcept {
    const uint32_t numSlots = ivars.runtime.txSlotProvider.numSlots;
    auto* metadataRing = ivars.runtime.txSlotProvider.metadataRing;
    if (numSlots == 0 || metadataRing == nullptr) {
        return;
    }

    // Seed the shared metadata ring with the pump's full lead before the IT DMA
    // context is started, so the first refill interrupt never meets an
    // uncommitted slot (which would fatally stop the context — channel never
    // reaches the wire).
    //
    // Critically, this runs before RX cadence acquisition and before any
    // OUTPUT_LAST completion can provide a packet execution anchor. Saffire's
    // StartStreams/FillFirewireBuffers path likewise pre-fills with NO_INFO
    // until ReadFirewireBuffers establishes the RX cadence history.
    // The packetizer owns DBC continuity, so the prefill-to-live handoff stays
    // gapless.
    ASFW::Protocols::Audio::AMDTP::AmdtpTimingState timing{};
    timing.txClockValid = false;

    uint32_t prepared = 0;
    for (uint64_t packetIndex = 0; packetIndex < kTxPumpLeadPackets; ++packetIndex) {
        if (!ivars.runtime.txStreamEngine.PrepareNextTransmitSlot(
                static_cast<uint32_t>(packetIndex), timing)) {
            break;
        }
        ++prepared;
    }

    ASFW_LOG(DirectAudio,
             "ADK DBG TX prefill seeded %u NO_INFO packets before isoch start (lead=%llu, model left unseeded)",
             prepared,
             kTxPumpLeadPackets);
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

        // Run the transmit pump to keep the timeline exposed ahead of the hardware
        const auto* txControl = ivars->runtime.txSlotProvider.controlBlock;
        if (txControl) {
            const uint64_t completionCursor = txControl->completionCursor.load(std::memory_order_relaxed);
            const uint64_t exposeCursor = txControl->exposeCursor.load(std::memory_order_relaxed);
            const uint64_t targetPacketIndex = completionCursor + kTxPumpLeadPackets;
            constexpr uint32_t kMaxPreparePerCall = 64;
            (void)ASFW::Audio::DriverKit::PrepareTransmitSlots(
                *ivars, exposeCursor, targetPacketIndex, kMaxPreparePerCall);
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
