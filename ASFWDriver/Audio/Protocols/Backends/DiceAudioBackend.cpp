// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "DiceAudioBackend.hpp"
#include "DiceRuntimeDeviceConfig.hpp"

#include "../../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../../Logging/Logging.hpp"
#include "../../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "../DICE/Core/DiceNotificationRouter.hpp"
#include "../DICE/Core/DICETypes.hpp"

#include <algorithm>
#include "../Duplex/FamilyDriver.hpp"
#include "../IDeviceProtocol.hpp"
#include "../StreamGeometryResolver.hpp"
#include "../DeviceProtocolChoice.hpp"
#include "../../DriverKit/Config/AudioDriverConfig.hpp"
#include "../../DriverKit/Config/AudioProfileRegistry.hpp"
#include "../../DriverKit/Config/DICE/DiceDeviceProfile.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <atomic>
#include <memory>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <string>
#include <vector>

namespace ASFW::Audio {

namespace {

[[nodiscard]] uint64_t UptimeMilliseconds() noexcept {
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS ||
        timebase.denom == 0) {
        return 0;
    }
    const unsigned __int128 nanos =
        static_cast<unsigned __int128>(mach_absolute_time()) *
        timebase.numer / timebase.denom;
    return static_cast<uint64_t>(nanos / 1'000'000U);
}

// Report how the device's own DICE registers compare with the profile's
// compiled-in constants, for every playback (DICE RX) stream.
//
// These are the two descriptions of the same streams that used to be consumed
// by different layers without ever meeting: DuplexStreamProfile reserves isoch
// bandwidth from the caps, while ASFWAudioDevice::StartIO frames CIP from the
// profile. A disagreement therefore shipped correctly-reserved bandwidth
// carrying wrongly-framed packets, and the symptom was silence with nothing
// logged. This is the first place both sides are in scope, so it is where they
// get compared. See StreamGeometryResolver.hpp for the precedence rule and why
// it is the device's.
//
// Stage 4: this now RESOLVES rather than reports. It answers both directions
// -- capture was previously never compared at all -- and returns a
// ResolvedDeviceGeometry whose Usable() is the refusal the resolver's contract
// always required.
//
// The verdict is not yet enforced at the start path: the consumers
// (AudioStreamProfile::Tx/RxChannelCount and ASFWAudioDevice::StartIO's
// BuildTxStreamConfig call) still read the profile, and switching them is the
// next step. Until then a disagreement is a loud, named refusal in the log
// rather than a silent mis-framing.

[[nodiscard]] constexpr uint32_t ClampStreamCountToHost(uint32_t count) noexcept {
    return (count < kMaxAudioStreamsPerDirection) ? count : kMaxAudioStreamsPerDirection;
}

[[nodiscard]] WireStreamGeometry PlaybackGeometryFromDevice(
    const AudioStreamRuntimeCaps& caps, uint32_t index) noexcept {
    if (index >= ClampStreamCountToHost(caps.hostToDeviceStreamCount)) {
        return {};
    }
    const auto& wire = caps.hostToDeviceStreams[index];
    return {.pcmChannels = wire.pcmChannels,
            .am824Slots = wire.am824Slots,
            .midiPorts = wire.midiPorts};
}

// Seeding a direction means "let the device's answer win without argument".
// That is only SAFE where the device's answer actually reaches the code that
// frames packets. Both directions now qualify:
//
//   - Bandwidth and transport geometry come from the device in both directions
//     (DuplexStreamProfile::Build reads caps.{deviceToHost,hostToDevice}Streams).
//   - CAPTURE encoding: IsochDuplexHostTransport hands caps-derived per-stream
//     geometry to DirectAudioReceiveConsumer.
//   - PLAYBACK encoding and packet allocation: the resolved per-stream geometry
//     is published across the nub (Audio/Model/AudioPropertyKeys.hpp) and
//     ASFWAudioDevice::StartIO builds each stream from it via
//     BuildResolvedTxStreamConfig, keeping only the framing constants the DICE
//     registers do not hold.
//
// Playback was false until that last line was true. While it was, a seeded
// playback geometry disagreeing with the device would have been waved through
// at publication and then mis-framed -- the recorded Venice F24 carries 16 + 8
// while its F32 profile says 16 + 16, so stream 1 was built at 16 channels /
// DBS 16 into an 8-slot stream. It now resolves to 16 + 8 end to end.
//
// These stay as named constants rather than being deleted: they are the
// statement of WHICH directions have been migrated, and the next family to move
// off profile constants needs the same question asked of it.
inline constexpr bool kPlaybackEncodingIsDeviceSourced = true;
inline constexpr bool kCaptureEncodingIsDeviceSourced = true;

/// Playback streams ASFWAudioDevice::StartIO can allocate: one primary plus one
/// secondary. Kept here as well so publication and start refuse the same
/// devices; StartIO carries the matching bound.
inline constexpr uint32_t kMaxPlaybackStreamsSupported = 2;

// Does this profile ASSERT the direction's geometry, or only seed it? The
// resolver takes a bool (it is deliberately free of profile headers), so this
// is the one place the profile's enum is read.
[[nodiscard]] bool AssertsGeometry(
    ASFW::Isoch::Audio::StreamGeometryAuthority authority) noexcept {
    return authority == ASFW::Isoch::Audio::StreamGeometryAuthority::kAsserted;
}

// Capture: honour what the profile declares -- the device already drives
// capture framing, so accepting its answer costs nothing.
[[nodiscard]] bool CaptureGeometryIsAsserted(
    const ASFW::Isoch::Audio::DICE::IDiceDeviceProfile& profile) noexcept {
    return TreatProfileAsAsserted(AssertsGeometry(profile.CaptureGeometryAuthority()),
                                  kCaptureEncodingIsDeviceSourced);
}

// Playback: a declared seed is treated as an ASSERTION while framing still
// reads the profile, so a disagreement refuses publication instead of shipping
// a mis-framed stream. The profile's own declaration is left untouched -- it
// describes what its constants MEAN; this decides what we can safely act on.
[[nodiscard]] bool PlaybackGeometryIsAsserted(
    const ASFW::Isoch::Audio::DICE::IDiceDeviceProfile& profile) noexcept {
    return TreatProfileAsAsserted(AssertsGeometry(profile.PlaybackGeometryAuthority()),
                                  kPlaybackEncodingIsDeviceSourced);
}

[[nodiscard]] WireStreamGeometry PlaybackGeometryFromProfile(
    const ASFW::Isoch::Audio::DICE::IDiceDeviceProfile& profile, uint32_t index) noexcept {
    if (!PlaybackGeometryIsAsserted(profile)) {
        return {};
    }
    ASFW::Isoch::Audio::AudioStreamConfig config{};
    if (index >= ClampStreamCountToHost(profile.TxStreamCount()) ||
        !profile.BuildTxStreamConfig(index, config)) {
        return {};
    }
    return {.pcmChannels = config.pcmChannels,
            .am824Slots = config.dbs,
            .midiPorts = config.midiSlots};
}

[[nodiscard]] WireStreamGeometry CaptureGeometryFromDevice(
    const AudioStreamRuntimeCaps& caps, uint32_t index) noexcept {
    if (index >= ClampStreamCountToHost(caps.deviceToHostStreamCount)) {
        return {};
    }
    const auto& wire = caps.deviceToHostStreams[index];
    return {.pcmChannels = wire.pcmChannels,
            .am824Slots = wire.am824Slots,
            .midiPorts = wire.midiPorts};
}

// Indexed, like the playback side: a profile describing unequal capture streams
// is compared stream by stream rather than every stream against stream 0.
// Using the default config here would have made an asymmetric profile report a
// correct aggregate while the resolver silently compared the wrong shapes.
// ASFW's profile naming inverts: Rx* is host capture.
[[nodiscard]] WireStreamGeometry CaptureGeometryFromProfile(
    const ASFW::Isoch::Audio::DICE::IDiceDeviceProfile& profile, uint32_t index) noexcept {
    if (!CaptureGeometryIsAsserted(profile)) {
        return {};
    }
    ASFW::Isoch::Audio::AudioStreamConfig config{};
    if (index >= ClampStreamCountToHost(profile.RxStreamCount()) ||
        !profile.BuildRxStreamConfig(index, config)) {
        return {};
    }
    return {.pcmChannels = config.pcmChannels,
            .am824Slots = config.dbs,
            .midiPorts = config.midiSlots};
}

// Log one direction's resolution. The resolution itself is
// ResolveDirectionGeometry in StreamGeometryResolver.hpp, which the host suite
// drives directly; this only reports what it decided.
void LogDirection(const char* directionName,
                  uint64_t guid,
                  const ResolvedDirectionGeometry& resolved) {
    if (resolved.count.disagrees) {
        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend: %{public}s stream COUNT disagrees device=%u profile=%u "
                       "GUID=0x%016llx - the transport arms the device's count while StartIO arms "
                       "the profile's, so one of them is wrong",
                       directionName, resolved.count.deviceStated,
                       resolved.count.profileStated, guid);
    }
    for (uint32_t i = 0; i < resolved.StreamCount() && i < kMaxResolvedStreams; ++i) {
        const auto& decision = resolved.streams[i];
        if (decision.disagrees) {
            ASFW_LOG_ERROR(Audio,
                           "DiceAudioBackend: %{public}s stream %u geometry disagrees "
                           "device=(pcm=%u dbs=%u midi=%u) profile=(pcm=%u dbs=%u midi=%u) "
                           "GUID=0x%016llx",
                           directionName, i,
                           decision.deviceStated.pcmChannels, decision.deviceStated.am824Slots,
                           decision.deviceStated.midiPorts,
                           decision.profileStated.pcmChannels, decision.profileStated.am824Slots,
                           decision.profileStated.midiPorts,
                           guid);
            continue;
        }
        ASFW_LOG(Audio,
                 "DiceAudioBackend: %{public}s stream %u geometry pcm=%u dbs=%u midi=%u source=%{public}s",
                 directionName, i,
                 decision.geometry.pcmChannels,
                 decision.geometry.am824Slots,
                 decision.geometry.midiPorts,
                 decision.source == StreamGeometrySource::kDevice ? "device" : "profile");
    }
}

// Both directions, resolved once. Capture was previously never compared at
// all -- only playback was -- so half the geometry crossed into the transport
// unchecked.
[[nodiscard]] ResolvedDeviceGeometry ResolveDeviceStreamGeometry(
    uint64_t guid,
    const AudioStreamRuntimeCaps& caps,
    const ASFW::Isoch::Audio::DICE::IDiceDeviceProfile& profile) {
    static_assert(kMaxResolvedStreams == kMaxAudioStreamsPerDirection,
                  "resolver bound drifted from the host array bound");
    static_assert(kMaxResolvedStreams == ASFW::Isoch::Audio::kMaxConfiguredStreams,
                  "resolver bound drifted from the nub's per-stream array bound");

    ResolvedDeviceGeometry resolved{};
    resolved.capture = ResolveDirectionGeometry(
        caps.deviceToHostStreamCount,
        ProfileStatedStreamCount(CaptureGeometryIsAsserted(profile), profile.RxStreamCount()),
        [&caps](uint32_t i) { return CaptureGeometryFromDevice(caps, i); },
        [&profile](uint32_t i) { return CaptureGeometryFromProfile(profile, i); });
    resolved.playback = ResolveDirectionGeometry(
        caps.hostToDeviceStreamCount,
        ProfileStatedStreamCount(PlaybackGeometryIsAsserted(profile), profile.TxStreamCount()),
        [&caps](uint32_t i) { return PlaybackGeometryFromDevice(caps, i); },
        [&profile](uint32_t i) { return PlaybackGeometryFromProfile(profile, i); });

    LogDirection("capture", guid, resolved.capture);
    LogDirection("playback", guid, resolved.playback);

    if (!resolved.Usable()) {
        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend: resolved geometry UNUSABLE GUID=0x%016llx "
                       "capture(usable=%u streams=%u pcm=%u firstBad=%u) "
                       "playback(usable=%u streams=%u pcm=%u firstBad=%u) - bandwidth would be "
                       "reserved from one description while CIP is framed from the other",
                       guid,
                       resolved.capture.Usable() ? 1U : 0U,
                       resolved.capture.StreamCount(),
                       resolved.capture.TotalPcmChannels(),
                       resolved.capture.FirstDisagreeingStream(),
                       resolved.playback.Usable() ? 1U : 0U,
                       resolved.playback.StreamCount(),
                       resolved.playback.TotalPcmChannels(),
                       resolved.playback.FirstDisagreeingStream());
    } else {
        ASFW_LOG(Audio,
                 "DiceAudioBackend: resolved geometry GUID=0x%016llx capture=%uch/%ustreams "
                 "playback=%uch/%ustreams",
                 guid,
                 resolved.capture.TotalPcmChannels(), resolved.capture.StreamCount(),
                 resolved.playback.TotalPcmChannels(), resolved.playback.StreamCount());
    }
    return resolved;
}

} // namespace

DiceAudioBackend::DiceAudioBackend(AudioNubPublisher& publisher,
                                   Discovery::DeviceRegistry& registry,
                                   AudioRuntimeRegistry& runtime,
                                   Session::AudioSessions& sessions,
                                   Driver::HardwareInterface& hardware,
                                   DICE::DiceNotificationRouter& notifications) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , runtime_(runtime)
    , hardware_(hardware)
    , sessions_(sessions)
    , notifications_(notifications) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: Failed to allocate lock");
    }

    IODispatchQueue* queue = nullptr;
    const kern_return_t kr = IODispatchQueue::Create("com.asfw.audio.dice", 0, 0, &queue);
    if (kr == kIOReturnSuccess && queue) {
        workQueue_ = OSSharedPtr(queue, OSNoRetain);
    } else {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: Failed to create work queue (0x%x)", kr);
    }

    notifications_.SetObserver(this, &DiceAudioBackend::NotificationObserverThunk);
}

DiceAudioBackend::~DiceAudioBackend() noexcept {
    // Stage 2b D1: defensive teardown in the destructor, matching MotuAudioBackend's
    // shape. Normal lifecycle calls BeginTeardown() explicitly before destruction;
    // this only covers a destructor-only path. Idempotent by exchange latch.
    BeginTeardown();
    notifications_.ClearObserver(this);
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void DiceAudioBackend::BeginTeardown() noexcept {
    stopping_.store(true, std::memory_order_release);
    notifications_.ClearObserver(this);

    if (teardownComplete_.load(std::memory_order_acquire)) {
        return;
    }

    if (!teardownStarted_.exchange(true, std::memory_order_acq_rel)) {
        // Atomically close admission and wait for any admitted in-flight publication to finish.
        publicationGate_.CloseAndWait();

        const uint64_t recoveryRejectBefore =
            recoveryRejectCount_.load(std::memory_order_acquire);
        const uint64_t probeRejectBefore =
            probeRejectCount_.load(std::memory_order_acquire);
        const uint64_t probeAbortBefore =
            probeAbortCount_.load(std::memory_order_acquire);
        const uint64_t coordinatorAbortBefore =
            sessions_.TeardownAbortCount();
        const uint64_t publicationRejectBefore =
            publicationGate_.RejectCount();
        const uint64_t startMs = UptimeMilliseconds();

        ASFW_LOG(Audio,
                 "DiceAudioBackend: BeginTeardown stopping=true draining dice queue");

        if (workQueue_) {
#ifdef ASFW_HOST_TEST
            if (onTeardownDrainStartedHookForTesting_) {
                onTeardownDrainStartedHookForTesting_();
            }
            workQueue_->DispatchSync([] {});
#else
            workQueue_->DispatchSync(^{});
#endif
        }

        const uint64_t endMs = UptimeMilliseconds();
        const uint64_t drainMs = endMs >= startMs ? endMs - startMs : 0;
        const uint64_t coordinatorAborted =
            sessions_.TeardownAbortCount() - coordinatorAbortBefore;
        const uint64_t probeAborted =
            probeAbortCount_.load(std::memory_order_acquire) - probeAbortBefore;
        const uint64_t recoveryRejected =
            recoveryRejectCount_.load(std::memory_order_acquire) - recoveryRejectBefore;
        const uint64_t probeRejected =
            probeRejectCount_.load(std::memory_order_acquire) - probeRejectBefore;

        ASFW_LOG(Audio,
                 "DiceAudioBackend: dice queue drained aborted=%llu recoveryRejected=%llu probeRejected=%llu publicationRejected=%llu drain=%llums",
                 coordinatorAborted + probeAborted,
                 recoveryRejected,
                 probeRejected,
                 publicationGate_.RejectCount() - publicationRejectBefore,
                 drainMs);

        teardownComplete_.store(true, std::memory_order_release);
        return;
    }

    // Secondary / concurrent callers wait safely until the primary drain is finished.
#ifdef ASFW_HOST_TEST
    if (onSecondaryTeardownWaitingHookForTesting_) {
        onSecondaryTeardownWaitingHookForTesting_();
    }
#endif
    while (!teardownComplete_.load(std::memory_order_acquire)) {
        IOSleep(1);
    }
}

void DiceAudioBackend::OnDeviceRecordUpdated(uint64_t guid) noexcept {
    EnsureNubForGuid(guid);
}

void DiceAudioBackend::CancelRemoteDeviceWork(uint64_t guid) noexcept {
    if (guid == 0) return;

    if (lock_) {
        IOLockLock(lock_);
        attemptsByGuid_.erase(guid);
        retryOutstanding_.erase(guid);
        activeStreamingGuids_.erase(guid);
        recoveringGuids_.erase(guid);
        IOLockUnlock(lock_);
    }

    ASFW_LOG(Audio,
             "DiceAudioBackend: remote-device work cancelled GUID=0x%016llx",
             guid);
}

void DiceAudioBackend::OnDeviceResumed(uint64_t guid) noexcept {
    ASFW_LOG(Audio,
             "AudioCoordinator: Device resumed while active; scheduling DICE recovery GUID=0x%016llx",
             guid);
    HandleRecoveryEvent(guid, DuplexRestartReason::kBusResetRebind);
}

void DiceAudioBackend::OnStreamsRestarted(uint64_t guid) noexcept {
    EnsureNubForGuid(guid);
}

void DiceAudioBackend::HandleHostTimingLoss(uint64_t guid) noexcept {
    HandleRecoveryEvent(guid, DuplexRestartReason::kRecoverAfterTimingLoss);
}

void DiceAudioBackend::HandleCycleInconsistent(uint64_t guid) noexcept {
    ASFW_LOG_WARNING(Audio,
                     "AudioCoordinator: cycleInconsistent observed; scheduling DICE recovery GUID=0x%016llx",
                     guid);
    HandleRecoveryEvent(guid, DuplexRestartReason::kRecoverAfterCycleInconsistent);
}

void DiceAudioBackend::HandleRecoveryEvent(uint64_t guid, DuplexRestartReason reason) noexcept {
    if (guid == 0) {
        return;
    }

    if (stopping_.load(std::memory_order_acquire) || sessions_.IsCancelled(guid)) {
        recoveryRejectCount_.fetch_add(1, std::memory_order_acq_rel);
        ASFW_LOG(Audio,
                 "DiceAudioBackend: recovery event ignored by lifecycle cancellation "
                 "GUID=%llx reason=%u",
                 guid,
                 static_cast<unsigned>(reason));
        return;
    }

    // A runtime fault (timing loss, cycle-inconsistent, ...) belongs to the run
    // that was streaming when it fired. CoreAudio re-probes a rate with rapid
    // StartIO/StopIO cycles, and each ordered teardown trips the same replay
    // detectors as a genuine mid-run fault; the session drops a fault whose run
    // has ended, or that fired while no run was confirmed. A device config
    // change is tied to its run the same way: the one our own CLOCK_SELECT
    // causes arrives while the session reconciles, names no running run, and is
    // dropped, as TCAT ignores it while a restart is running. Bus-reset rebinds
    // are topology events and always restart.
    const bool isRuntimeFault =
        reason == DuplexRestartReason::kRecoverAfterTimingLoss ||
        reason == DuplexRestartReason::kRecoverAfterCycleInconsistent ||
        reason == DuplexRestartReason::kRecoverAfterLockLoss ||
        reason == DuplexRestartReason::kRecoverAfterTxFault;
    const bool tiedToRun =
        isRuntimeFault || reason == DuplexRestartReason::kDeviceConfigChange;
    const uint64_t observedRun = tiedToRun ? sessions_.RunningRun(guid) : 0;

    if (!TryBeginRecovery(guid)) {
        return;
    }

    auto recover = ^{
        // FW-61: a block enqueued just before BeginTeardown's drain bails here before any
        // MMIO, so it cannot run after ASFWDriver::Stop detaches hardware.
        if (stopping_.load(std::memory_order_acquire) || sessions_.IsCancelled(guid)) {
            recoveryRejectCount_.fetch_add(1, std::memory_order_acq_rel);
            ASFW_LOG(Audio,
                     "DiceAudioBackend: queued recovery aborted by lifecycle cancellation "
                     "GUID=%llx reason=%u",
                     guid,
                     static_cast<unsigned>(reason));
            FinishRecovery(guid);
            return;
        }

        // Health gate: a host-side replay discontinuity (aggregate-device
        // StartIO/StopIO churn, an RX packet gap) fires the same timing-loss
        // detector as a genuine device clock drop. When the device still reports
        // a locked, healthy clock, the RX epoch reset
        // (ResetReplayEpochForDiscontinuity) already re-establishes cadence and
        // replay; a restart would only tear down a healthy session. A read
        // failure returns false: never suppress a recovery on missing evidence.
        if (isRuntimeFault && DeviceReportsHealthyClock(guid)) {
            recoveryRejectCount_.fetch_add(1, std::memory_order_acq_rel);
            ASFW_LOG(Audio,
                     "DiceAudioBackend: runtime-fault recovery dropped (device clock "
                     "locked+healthy; RX self-heals) GUID=%llx reason=%u",
                     guid,
                     static_cast<unsigned>(reason));
            FinishRecovery(guid);
            return;
        }

        // DICE restarts after a quiet period: success means the restart is
        // queued; OnStreamsRestarted runs once it has happened.
        const IOReturn status = sessions_.RequestRestart(guid, reason, observedRun);
        if (status == kIOReturnSuccess) {
            ASFW_LOG(Audio,
                     "DiceAudioBackend: Recovery requested GUID=%llx reason=%u",
                     guid,
                     static_cast<unsigned>(reason));
        } else if (status == kIOReturnUnsupported || status == kIOReturnAborted) {
            // The session declined: nothing should run, or the fault is stale.
            // Neither a success to announce nor a failure to escalate (FW-146).
            ASFW_LOG(Audio,
                     "DiceAudioBackend: Recovery not applicable GUID=%llx reason=%u kr=0x%x",
                     guid,
                     static_cast<unsigned>(reason),
                     status);
        } else {
            ASFW_LOG_ERROR(Audio,
                           "DiceAudioBackend: Recovery failed GUID=%llx reason=%u kr=0x%x",
                           guid,
                           static_cast<unsigned>(reason),
                           status);
        }
        FinishRecovery(guid);
    };

    if (workQueue_) {
        workQueue_->DispatchAsync(recover);
        return;
    }

    recover();
}

void DiceAudioBackend::HandleDeviceNotification(uint64_t guid, uint32_t bits) noexcept {
    // The device changed its stream configuration: restart the running
    // streams on the new one (TCAT NotificationWriteCallback, AUDIO_SESSION_REDESIGN.md §2.4).
    if ((bits & (DICE::Notify::kRxConfigChange | DICE::Notify::kTxConfigChange)) != 0) {
        HandleRecoveryEvent(guid, DuplexRestartReason::kDeviceConfigChange);
    }

    if ((bits & (DICE::Notify::kLockChange | DICE::Notify::kExtStatus)) == 0) {
        return;
    }

    if (stopping_.load(std::memory_order_acquire)) {
        probeRejectCount_.fetch_add(1, std::memory_order_acq_rel);
        ASFW_LOG(Audio,
                 "DiceAudioBackend: device notification ignored by teardown GUID=%llx bits=0x%08x",
                 guid, bits);
        return;
    }

    // Only a streaming device's clock health matters here; an idle device
    // re-reads its clock at the next start.
    if (!sessions_.IsStreaming(guid)) {
        return;
    }

    auto probe = ^{
        if (stopping_.load(std::memory_order_acquire) ||
            sessions_.IsCancelled(guid)) {
            probeRejectCount_.fetch_add(1, std::memory_order_acq_rel);
            ASFW_LOG(Audio,
                     "DiceAudioBackend: queued health probe ignored by lifecycle cancellation "
                     "GUID=%llx bits=0x%08x",
                     guid,
                     bits);
            return;
        }
        ProbeDuplexHealth(guid, bits);
    };

    if (workQueue_) {
        workQueue_->DispatchAsync(probe);
    } else {
        probe();
    }
}

void DiceAudioBackend::ProbeDuplexHealth(uint64_t guid, uint32_t notificationBits) noexcept {
    if (stopping_.load(std::memory_order_acquire) ||
        sessions_.IsCancelled(guid)) {
        probeRejectCount_.fetch_add(1, std::memory_order_acq_rel);
        ASFW_LOG(Audio,
                 "DiceAudioBackend: health probe refused by lifecycle cancellation "
                 "GUID=%llx bits=0x%08x",
                 guid,
                 notificationBits);
        return;
    }

    // Hold a shared_ptr for the duration of the (blocking) health probe so the
    // protocol cannot be torn down underneath us by a concurrent device removal.
    auto protocol = runtime_.FindShared(guid);
    auto* family = protocol ? protocol->AsFamilyDriver() : nullptr;
    if (!family) {
        return;
    }
    if (stopping_.load(std::memory_order_acquire) ||
        sessions_.IsCancelled(guid)) {
        probeRejectCount_.fetch_add(1, std::memory_order_acq_rel);
        ASFW_LOG(Audio,
                 "DiceAudioBackend: health probe refused by lifecycle cancellation before read "
                 "GUID=%llx bits=0x%08x",
                 guid,
                 notificationBits);
        return;
    }

    // Blocks up to kHealthBridgeTimeoutMs; service teardown aborts it through
    // the family's teardown token.
    const auto health = family->ReadHealth(kHealthBridgeTimeoutMs);
    const IOReturn probeStatus = health ? kIOReturnSuccess : health.error();

    if (probeStatus == kIOReturnAborted &&
        (stopping_.load(std::memory_order_acquire) || sessions_.IsCancelled(guid))) {
        // Lifecycle abort, which is what probeAbortCount_ measures and what the
        // BeginTeardown drain summary reports. A device-side abort while the
        // backend runs normally is reported below as a failed probe instead.
        probeAbortCount_.fetch_add(1, std::memory_order_acq_rel);
        ASFW_LOG(Audio,
                 "DiceAudioBackend: health probe aborted by lifecycle cancellation "
                 "GUID=%llx bits=0x%08x kr=0x%x",
                 guid,
                 notificationBits,
                 kIOReturnAborted);
        return;
    }

    if (probeStatus == kIOReturnTimeout) {
        ASFW_LOG_WARNING(Audio,
                         "DiceAudioBackend: health probe timed out GUID=%llx bits=0x%08x",
                         guid,
                         notificationBits);
        return;
    }

    if (probeStatus != kIOReturnSuccess) {
        ASFW_LOG_WARNING(Audio,
                         "DiceAudioBackend: health probe failed GUID=%llx bits=0x%08x kr=0x%x",
                         guid,
                         notificationBits,
                         probeStatus);
        return;
    }

    const bool sourceLocked = health->sourceLocked;
    const bool extClockHealthy = health->clockReferenceHealthy;

    char notifyStr[96];
    char clockStr[40];
    char extStr[128];
    DICE::FormatNotification(notificationBits, notifyStr, sizeof(notifyStr));
    DICE::FormatGlobalStatus(health->status, clockStr, sizeof(clockStr));
    DICE::FormatExtStatus(health->extStatus, extStr, sizeof(extStr));

    if (sourceLocked && extClockHealthy) {
        // Healthy — but the device may have moved to a different rate on its
        // own (front-panel clock change / external sync source). Compare the
        // PLL's locked nominal rate against the host's current belief and, on
        // a mismatch, tell the audio driver to re-sync the HAL (forced format
        // change; AppleUSBAudio's device-driven rate-move analog).
        const uint32_t deviceRateHz = health->nominalRateHz;
        auto* nub = publisher_.GetNub(guid);
        const uint32_t hostRateHz = nub ? nub->GetCurrentSampleRateHz() : 0;
        if (nub && deviceRateHz != 0 && hostRateHz != 0 &&
            deviceRateHz != hostRateHz) {
            // A mismatch here is only device-initiated if the host isn't the
            // one moving the clock. During a host-initiated rate change the
            // PLL relocks at the new rate while the nub's belief still holds
            // the old one (it updates only after RequestClockConfig returns),
            // and the device's lock-change notifications land exactly in that
            // window. Notifying then would inject a second, competing
            // config-change into the middle of the host's own change (HAL
            // rate switches wedge until the client reopens the device).
            // Suppress while the session reconciles or holds an unapplied
            // clock change, and when the "new" device rate is just the echo
            // of the clock the host itself asked for.
            const auto session = sessions_.Snapshot(guid);
            const bool echoesHostClock =
                session.has_value() &&
                (session->clockChangePending ||
                 session->desiredClock.sampleRateHz == deviceRateHz);
            if (echoesHostClock || sessions_.IsReconciling(guid)) {
                ASFW_LOG_RL(Audio, "dice/rate-echo", 1000, OS_LOG_TYPE_DEFAULT,
                            "DiceAudioBackend: rate mismatch is host-initiated "
                            "(in flight) GUID=%llx device=%u Hz host=%u Hz -> no resync",
                            guid, deviceRateHz, hostRateHz);
                return;
            }
            ASFW_LOG_WARNING(Audio,
                             "DiceAudioBackend: device-initiated clock change "
                             "GUID=%llx device=%u Hz host=%u Hz -> notify audio driver",
                             guid, deviceRateHz, hostRateHz);
            nub->NotifyDeviceClockChanged(deviceRateHz);
            return;
        }

        // The device is just narrating its clock/ext status. Surface what it
        // actually reports (rate-limited) instead of staying silent.
        ASFW_LOG_RL(Audio, "dice/notify-confirm", 1000, OS_LOG_TYPE_DEFAULT,
                    "DiceAudioBackend: notify confirm GUID=%llx notify=%{public}s clock=%{public}s ext=%{public}s healthy",
                    guid, notifyStr, clockStr, extStr);
        return;
    }

    ASFW_LOG_WARNING(Audio,
                     "DiceAudioBackend: lock health DEGRADED GUID=%llx notify=%{public}s clock=%{public}s ext=%{public}s sourceLocked=%u extHealthy=%u -> recover",
                     guid, notifyStr, clockStr, extStr, sourceLocked, extClockHealthy);

    (void)guid;
    (void)notificationBits;
}

bool DiceAudioBackend::DeviceReportsHealthyClock(uint64_t guid) noexcept {
    if (stopping_.load(std::memory_order_acquire) ||
        sessions_.IsCancelled(guid)) {
        return false;
    }
    // Hold the protocol alive for the blocking read (same discipline as
    // ProbeDuplexHealth) so a concurrent device removal cannot free it underneath.
    auto protocol = runtime_.FindShared(guid);
    auto* family = protocol ? protocol->AsFamilyDriver() : nullptr;
    if (!family) {
        return false;
    }

    const auto health = family->ReadHealth(kHealthBridgeTimeoutMs);
    return health && health->sourceLocked && health->clockReferenceHealthy;
}

bool DiceAudioBackend::TryBeginRecovery(uint64_t guid) noexcept {
    if (!lock_) return false;

    IOLockLock(lock_);
    if (recoveringGuids_.find(guid) != recoveringGuids_.end()) {
        IOLockUnlock(lock_);
        return false;
    }
    recoveringGuids_.insert(guid);
    IOLockUnlock(lock_);
    return true;
}

void DiceAudioBackend::FinishRecovery(uint64_t guid) noexcept {
    if (!lock_) return;

    IOLockLock(lock_);
    recoveringGuids_.erase(guid);
    IOLockUnlock(lock_);
}

void DiceAudioBackend::NotificationObserverThunk(void* context, uint64_t guid, uint32_t bits) noexcept {
    auto* self = static_cast<DiceAudioBackend*>(context);
    if (!self) {
        return;
    }
    self->HandleDeviceNotification(guid, bits);
}

void DiceAudioBackend::EnsureNubForGuid(uint64_t guid) noexcept {
    if (guid == 0) return;

    auto admission = std::make_shared<PublicationGate::AdmissionScope>(publicationGate_);
    if (!admission->IsAdmitted()) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend: publication refused by teardown GUID=0x%016llx",
                 guid);
        return;
    }

#ifdef ASFW_HOST_TEST
    if (beforePublishHookForTesting_) {
        beforePublishHookForTesting_();
    }
#endif

    if (admission->AbortIfStopping()) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend: publication cancelled by concurrent teardown GUID=0x%016llx",
                 guid);
        return;
    }

    const auto record = registry_.SnapshotByGuid(guid);
    if (!record.has_value()) {
        ASFW_LOG(Audio, "DiceAudioBackend::EnsureNubForGuid: no registry record for GUID=0x%016llx", guid);
        return;
    }

    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(*record);
    if (!policy || !registry_.IsCurrent(policy->route) ||
        ChooseAudioBackend(policy->plan) != AudioBackendKind::Dice) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend::EnsureNubForGuid: skipping GUID=0x%016llx vendor=0x%06x "
                 "model=0x%06x (no current DICE policy for the route)",
                 guid, record->vendorId, record->modelId);
        return;
    }

    // Consume the immutable catalog decision installed by discovery. The route
    // token is retained across asynchronous geometry loading and checked again
    // before publication, so a stale callback cannot publish for a replaced
    // route.
    const auto route = policy->route;
    const auto choice = ChooseDeviceProtocol(policy->plan);
    const uint32_t profileBuilderId =
        choice.has_value() ? static_cast<uint32_t>(choice->builder) : 0U;
    const auto* profile =
        ASFW::Isoch::Audio::AudioProfileRegistry::DiceProfileForBuilderId(profileBuilderId);
    if (!profile) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend::EnsureNubForGuid: no isoch profile for GUID=0x%016llx "
                 "vendor=0x%06x model=0x%06x builder=%u",
                 guid, record->vendorId, record->modelId, profileBuilderId);
        return;
    }

    ASFW_LOG(Audio,
             "DiceAudioBackend::EnsureNubForGuid: matched profile=%{public}s for GUID=0x%016llx",
             profile->Name(), guid);

    auto protocol = runtime_.FindShared(guid);

    Model::ASFWAudioDevice dev{};
    dev.guid = record->guid;
    dev.vendorId = record->vendorId;
    dev.modelId = record->modelId;
    // Carry the catalog's answer to the audio side, which only ever sees
    // scalars and must not have to re-derive it.
    dev.profileBuilderId = profileBuilderId;
    dev.deviceName = profile->Name();
    dev.inputChannelCount = profile->RxChannelCount();
    dev.outputChannelCount = profile->TxChannelCount();
    dev.channelCount = std::max(dev.inputChannelCount, dev.outputChannelCount);
    dev.inputPlugName = "Input";
    dev.outputPlugName = "Output";
    // The rate set comes from the device once its caps are loaded (below);
    // 48 kHz is the rate bring-up programs by default.
    dev.currentSampleRate = 48000u;

    // Enrich with the device's real per-channel labels (if the protocol has
    // loaded them), update the endpoint runtime, then publish the nub. Host
    // input == device TX, host output == device RX (see AudioTypes.hpp), which
    // is exactly how GetChannelLabels reports them.
    auto finish = [this, guid, route, profile, admission](Model::ASFWAudioDevice dev,
                                                   const std::shared_ptr<IDeviceProtocol>& protocol) {
        if (admission->AbortIfStopping()) {
            ASFW_LOG(Audio,
                     "DiceAudioBackend: publication cancelled by concurrent teardown GUID=0x%016llx",
                     guid);
            return;
        }
        if (!registry_.IsCurrent(route)) {
            ASFW_LOG(Audio,
                     "DiceAudioBackend: suppressing publication for stale route GUID=0x%016llx",
                     guid);
            return;
        }
#ifdef ASFW_HOST_TEST
        if (beforePublishHookForTesting_) {
            beforePublishHookForTesting_();
        }
#endif
        if (admission->AbortIfStopping()) {
            ASFW_LOG(Audio,
                     "DiceAudioBackend: publication cancelled by concurrent teardown GUID=0x%016llx",
                     guid);
            return;
        }
        if (!registry_.IsCurrent(route)) {
            ASFW_LOG(Audio,
                     "DiceAudioBackend: suppressing publication for stale route GUID=0x%016llx",
                     guid);
            return;
        }
        if (!protocol) {
            // No protocol means nothing can answer for the device's geometry,
            // so publishing here would ship exactly the unvalidated endpoint
            // the caps check below exists to prevent -- it just reached the
            // nub by a different door. Refuse; the device is not lost, because
            // StartStreaming and OnDeviceRecordUpdated both call back into
            // EnsureNubForGuid once a protocol exists.
            ASFW_LOG_ERROR(Audio,
                           "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                           "GUID=0x%016llx - no protocol, geometry cannot be validated",
                           guid);
            return;
        }
        {
            AudioStreamRuntimeCaps caps{};
            const bool haveCaps = protocol->GetRuntimeAudioStreamCaps(caps);

            if (!haveCaps) {
                // Without runtime caps the resolver cannot run, so the
                // publication would carry unvalidated profile-only geometry.
                // That is the class of silent mismatch this path exists to
                // prevent. Refuse and let the geometry-load callback retry.
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                               "GUID=0x%016llx - runtime caps unavailable, cannot validate geometry",
                               guid);
                return;
            }

            // Compare the device's registers with the profile's constants
            // before anything consumes either, and make the aggregate the HAL
            // is told match the streams the device actually carries.
            const auto resolvedGeometry =
                ResolveDeviceStreamGeometry(guid, caps, *profile);

            if (!resolvedGeometry.Usable()) {
                // Publishing anyway is what used to happen, and the symptom was
                // silence with nothing attributable: bandwidth reserved from one
                // description, CIP framed from the other. Refuse the endpoint
                // instead, so the failure names itself at publication.
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                               "GUID=0x%016llx - resolved stream geometry is unusable",
                               guid);
                return;
            }

            // The resolved totals are the authority for per-stream wire
            // geometry (stream counts, slot counts). The HAL-facing channel
            // counts, however, must respect the protocol's visibility policy:
            // TCAT deliberately sets hostInputPcmChannels to zero for devices
            // like Weiss INT202 whose capture streams should remain hidden from
            // CoreAudio, even though the physical wire carries them. When the
            // protocol reported zero, preserve that decision; otherwise adopt
            // the resolved sum, which accounts for asymmetric multi-stream
            // devices (e.g. Venice F24 with 16+8).
            const uint32_t resolvedCapturePcm = resolvedGeometry.capture.TotalPcmChannels();
            caps.hostInputPcmChannels = (caps.hostInputPcmChannels == 0)
                                            ? 0
                                            : resolvedCapturePcm;
            caps.hostOutputPcmChannels = resolvedGeometry.playback.TotalPcmChannels();
            caps.deviceToHostStreamCount = resolvedGeometry.capture.StreamCount();
            caps.hostToDeviceStreamCount = resolvedGeometry.playback.StreamCount();

            if (ApplyDiceRuntimeCapsToDeviceConfig(caps, dev)) {
                ASFW_LOG(Audio,
                         "DiceAudioBackend::EnsureNubForGuid: applied runtime geometry rate=%u in=%u out=%u (GUID=0x%016llx)",
                         dev.currentSampleRate,
                         dev.inputChannelCount,
                         dev.outputChannelCount,
                         guid);
            }

            // Publish the resolved per-stream geometry so the audio side can
            // frame packets from what the device reported. Without this it
            // falls back to the profile's constants, which describe a
            // DIFFERENT device whenever the streams are not all one width --
            // the recorded Venice F24 carries 16 + 8 playback against an F32
            // profile's 16 + 16.
            //
            // channelOffset is the RUNNING SUM of preceding stream widths, the
            // same base the vendor drivers carry
            // (AlesisFirewireAudioEngine::CreateStreams advances it by each
            // stream's own count). It is deliberately not index * width: those
            // coincide only while every stream is stream 0's width.
            const auto publishStreams =
                [](const ResolvedDirectionGeometry& direction,
                   std::vector<Model::ASFWAudioWireStream>& out) {
                    out.clear();
                    uint32_t channelOffset = 0;
                    for (uint32_t i = 0;
                         i < direction.StreamCount() && i < kMaxResolvedStreams; ++i) {
                        const auto& geometry = direction.streams[i].geometry;
                        out.push_back(Model::ASFWAudioWireStream{
                            .pcmChannels = geometry.pcmChannels,
                            .am824Slots = geometry.am824Slots,
                            .midiPorts = geometry.midiPorts,
                            .channelOffset = channelOffset,
                        });
                        channelOffset += geometry.pcmChannels;
                    }
                };
            publishStreams(resolvedGeometry.playback, dev.playbackStreams);
            publishStreams(resolvedGeometry.capture, dev.captureStreams);
            // A DICE device only reaches here with a usable verdict, so the
            // audio side must never substitute profile constants for what the
            // device reported. If the arrays go missing in transit, starting
            // fails rather than degrading.
            dev.resolvedGeometryRequired = !dev.playbackStreams.empty();

            // Offer exactly the rates the device supports, up to the validated
            // ceiling, as TCAT's kexts do (CLOCK_CAPABILITIES & 0x7F). A device
            // supporting none of them cannot stream in this build: refuse it
            // rather than publish rates it would reject.
            dev.sampleRates = DICE::DicePublishedRates(caps.deviceRateMask);
            if (dev.sampleRates.empty()) {
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                               "GUID=0x%016llx - device supports no rate up to %u Hz (rates=0x%02x)",
                               guid, DICE::kDiceMaxSupportedRateHz, caps.deviceRateMask);
                return;
            }
            dev.deviceSampleRates = true;
            if (std::find(dev.sampleRates.begin(), dev.sampleRates.end(), dev.currentSampleRate) ==
                dev.sampleRates.end()) {
                dev.currentSampleRate = dev.sampleRates.back();
            }

            // Refuse a device this build cannot actually arm, at publication
            // rather than at the first StartIO. ASFWAudioDevice allocates one
            // primary and one secondary TX stream, so a device carrying more
            // would publish an endpoint that fails every time CoreAudio tries
            // to start it -- a worse failure than never appearing, because it
            // looks like a broken device rather than an unsupported one.
            // StartIO enforces the same bound independently; this is the early,
            // attributable half.
            if (dev.playbackStreams.size() > kMaxPlaybackStreamsSupported) {
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                               "GUID=0x%016llx - device carries %zu playback streams, this "
                               "build configures at most %u",
                               guid, dev.playbackStreams.size(),
                               kMaxPlaybackStreamsSupported);
                return;
            }

            // Bandwidth reservation, capture layout and playback framing all
            // derive from the SAME protocol caps: DuplexStreamProfile::Build
            // reads them directly, and the geometry published here is those
            // caps after validation against the profile. They therefore agree
            // by construction rather than by coincidence.
            //
            // What is NOT implemented is refresh. AudioNubPublisher::EnsureNub
            // is create-once, so a later resolution does not reach a live nub.
            // That is safe only because DICETcatProtocol::ResetRuntimeCaps is
            // reachable only from Shutdown, so the geometry cannot change while
            // a nub exists. The two decisions are coupled: whichever change
            // makes caps re-readable (a rate change crossing a rate mode --
            // documentation/DICE_TCAT_ARCHITECTURE.md sec 4.2 step D) must also
            // make the nub refreshable, or the audio side keeps framing from a
            // description the device has stopped honouring.

            for (size_t i = 0; i < dev.playbackStreams.size(); ++i) {
                ASFW_LOG(Audio,
                         "DiceAudioBackend::EnsureNubForGuid: playback stream %zu pcm=%u dbs=%u "
                         "midi=%u offset=%u (GUID=0x%016llx)",
                         i, dev.playbackStreams[i].pcmChannels,
                         dev.playbackStreams[i].am824Slots,
                         dev.playbackStreams[i].midiPorts,
                         dev.playbackStreams[i].channelOffset, guid);
            }

            // Name the device now that its geometry is known. For a profile
            // serving one model this returns Name() unchanged; for a range
            // sharing one identity -- Midas Venice F16/F24/F32 -- the measured
            // capture width is the only thing that tells the variants apart,
            // and it is not available until here.
            if (const char* resolvedName =
                    profile->NameForGeometry(dev.inputChannelCount, dev.outputChannelCount)) {
                if (dev.deviceName != resolvedName) {
                    ASFW_LOG(Audio,
                             "DiceAudioBackend::EnsureNubForGuid: named from geometry "
                             "'%{public}s' -> '%{public}s' in=%u out=%u (GUID=0x%016llx)",
                             dev.deviceName.c_str(), resolvedName,
                             dev.inputChannelCount, dev.outputChannelCount, guid);
                    dev.deviceName = resolvedName;
                }
            }

            std::vector<std::string> inNames;
            std::vector<std::string> outNames;
            if (protocol->GetChannelLabels(inNames, outNames)) {
                if (!inNames.empty()) {
                    dev.inputChannelNames = std::move(inNames);
                }
                if (!outNames.empty()) {
                    dev.outputChannelNames = std::move(outNames);
                }
                ASFW_LOG(Audio,
                         "DiceAudioBackend::EnsureNubForGuid: applied device channel labels in=%zu out=%zu (GUID=0x%016llx)",
                         dev.inputChannelNames.size(), dev.outputChannelNames.size(), guid);
            }
        }
        // Validate before replacing the runtime snapshot. A rejected geometry
        // must not reach transport while the audio graph retains the old one.
        if (publisher_.GetNub(guid) != nullptr) {
            (void)publisher_.RefreshNubProperties(guid, dev, "DICE");
            return;
        }
        if (auto endpoint = runtime_.EnsureEndpointRuntime(guid)) {
            endpoint->UpdateConfig(dev);
        }
        (void)publisher_.EnsureNub(guid, dev, "DICE");
    };

    // Channel labels live in the TCAT stream-format name sections, cached only
    // once runtime caps load (during the first stream discovery). Load them
    // once before the first publish so CoreAudio shows the real names from the
    // start. The load early-returns if caps are already cached; a failed load
    // refuses publication in the callback below, and a missing protocol refuses
    // inside finish(). Both are explicit: there is no path to EnsureNub that
    // has not compared the device against the profile.
    if (protocol) {
        protocol->EnsureRuntimeStreamGeometry(
            [finish, dev, protocol, guid](IOReturn status) mutable {
                if (status != kIOReturnSuccess) {
                    // Refuse here rather than handing off to finish(). finish()
                    // only rejects when GetRuntimeAudioStreamCaps returns
                    // nothing, and caps cached by an EARLIER successful load
                    // survive a later failure -- so it could publish against a
                    // stale description while this line claimed otherwise.
                    ASFW_LOG_ERROR(Audio,
                                   "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                                   "GUID=0x%016llx - geometry load failed status=0x%08x",
                                   guid, status);
                    return;
                }
                finish(std::move(dev), protocol);
            });
        return;
    }
    finish(std::move(dev), protocol);
}

IOReturn DiceAudioBackend::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    if (stopping_.load(std::memory_order_acquire)) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend: StartStreaming refused by teardown GUID=0x%016llx",
                 guid);
        return kIOReturnAborted;
    }

    auto* nub = publisher_.GetNub(guid);
    if (!nub) {
        EnsureNubForGuid(guid);
        nub = publisher_.GetNub(guid);
        if (!nub) {
            return kIOReturnNotReady;
        }
    }

    auto endpoint = runtime_.FindEndpointRuntime(guid);
    if (!endpoint || !endpoint->HasCompleteDirectAudioMemory()) {
        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend: StartStreaming refused missing direct runtime/memory GUID=0x%016llx endpoint=%p",
                       guid,
                       endpoint.get());
        return kIOReturnNotReady;
    }

    const IOReturn status = sessions_.Attach(guid);
    if (status == kIOReturnSuccess) {
        EnsureNubForGuid(guid);
        if (lock_) {
            IOLockLock(lock_);
            activeStreamingGuids_.insert(guid);
            IOLockUnlock(lock_);
        }
    }
    return status;
}

IOReturn DiceAudioBackend::StopStreaming(uint64_t guid) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend: StopStreaming refused by teardown GUID=0x%016llx",
                 guid);
        if (lock_) {
            IOLockLock(lock_);
            activeStreamingGuids_.erase(guid);
            recoveringGuids_.erase(guid);
            IOLockUnlock(lock_);
        }
        return kIOReturnAborted;
    }

    const IOReturn status = sessions_.Detach(guid);
    if (status == kIOReturnSuccess && lock_) {
        IOLockLock(lock_);
        activeStreamingGuids_.erase(guid);
        recoveringGuids_.erase(guid);
        IOLockUnlock(lock_);
    }
    return status;
}

IOReturn DiceAudioBackend::RequestClockConfig(uint64_t guid,
                                              const AudioClockConfig& desiredClock,
                                              DuplexRestartReason reason) noexcept {
    if (stopping_.load(std::memory_order_acquire)) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend: RequestClockConfig refused by teardown GUID=0x%016llx",
                 guid);
        return kIOReturnAborted;
    }

    const IOReturn status = sessions_.ChangeClock(guid, desiredClock, reason);
    if (status == kIOReturnSuccess) {
        EnsureNubForGuid(guid);
    }
    return status;
}

} // namespace ASFW::Audio
