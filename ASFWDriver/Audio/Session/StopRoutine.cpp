// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "StopRoutine.hpp"
#include "SessionClock.hpp"

#include "../Protocols/Backends/DuplexStreamProfile.hpp"
#include "../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Audio::Session {

StopRoutine::StopRoutine(Discovery::DeviceRegistry& registry,
                         IIsochDuplexHostTransport& host,
                         const std::atomic<bool>* teardown,
                         std::atomic<uint64_t>& teardownAborts) noexcept
    : registry_(registry), host_(host), teardown_(teardown), teardownAborts_(teardownAborts) {}

bool StopRoutine::TeardownRequested() const noexcept {
    return teardown_ != nullptr && teardown_->load(std::memory_order_acquire);
}

void StopRoutine::RecordTeardownAbort(const char* stage, uint64_t guid) noexcept {
    teardownAborts_.fetch_add(1, std::memory_order_acq_rel);
    ASFW_LOG(Audio, "[Session] stop aborted by teardown stage=%{public}s GUID=%llx", stage, guid);
}

IOReturn StopRoutine::Run(uint64_t guid,
                          const Discovery::DeviceRecord& record,
                          FamilyDriver& family,
                          const AudioStreamRuntimeCaps& caps,
                          const AudioDuplexChannels& channels) noexcept {
    const auto trace = [&](const char* stage, auto&& action) {
        const uint64_t begin = UptimeMilliseconds();
        ASFW_LOG(Audio, "[StopTrace] guid=%016llx stage=%{public}s phase=begin", guid, stage);
        const IOReturn status = action();
        ASFW_LOG(Audio, "[StopTrace] guid=%016llx stage=%{public}s phase=end kr=0x%x (%{public}s) elapsedMs=%llu",
                 guid, stage, status, ASFW::Logging::IOReturnName(status), UptimeMilliseconds() - begin);
        return status;
    };
    // No MMIO after teardown: the service detaches the hardware next.
    if (TeardownRequested()) {
        RecordTeardownAbort("Stop", guid);
        return kIOReturnAborted;
    }

    const Backends::DuplexStreamProfile profile =
        Backends::DuplexStreamProfileResolver::Resolve(record, caps, channels);
    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(record);
    if (!profile.policyResolved || policy == nullptr || !registry_.IsCurrent(policy->route)) {
        // The route changed under this session. Stop local DMA, but do not infer
        // a device-side stop recipe from stale identity data.
        return trace("host-cleanup-reset", [&] { return host_.StopAllAfterBusReset(); });
    }

    if (profile.stopOrder.disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive) {
        // AV/C: break each PCR connection before stopping the host context that
        // fed it. A failed/uncertain BREAK must retain the channel reservation
        // while the current remote PCR may still reference it (TA 1999032 5.1.1).
        const IOReturn playback = family.DisconnectPlayback();
        const kern_return_t transmit = trace("host-tx", [&] { return host_.StopPreparedTransmit(); });
        const IOReturn capture = family.DisconnectCapture();
        const kern_return_t receive = trace("host-rx", [&] { return host_.StopPreparedReceive(); });
        if ((playback != kIOReturnSuccess || capture != kIOReturnSuccess) &&
            registry_.IsCurrent(policy->route)) {
            ASFW_LOG_ERROR(Audio,
                "[CmpReservationHeld] guid=0x%016llx playback=0x%08x (%{public}s) capture=0x%08x (%{public}s) tx=0x%08x (%{public}s) rx=0x%08x (%{public}s); remote disconnect unresolved",
                guid, playback, ASFW::Logging::IOReturnName(playback), capture, ASFW::Logging::IOReturnName(capture), transmit, ASFW::Logging::IOReturnName(transmit), receive, ASFW::Logging::IOReturnName(receive));
            return playback != kIOReturnSuccess ? playback : capture;
        }
        // The contexts are already stopped; StopAll releases the reservation and
        // the active GUID without another wire action.
        const kern_return_t cleanup = trace("host-cleanup-irm", [&] { return host_.StopAll(); });
        const IOReturn result = transmit != kIOReturnSuccess  ? transmit
                                : receive != kIOReturnSuccess ? receive
                                                              : cleanup;
        if (result != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio,
                           "[Session] stop failed GUID=0x%016llx tx=0x%08x (%{public}s) rx=0x%08x (%{public}s) "
                           "cleanup=0x%08x (%{public}s) -> 0x%08x (%{public}s)",
                           guid, transmit, ASFW::Logging::IOReturnName(transmit), receive, ASFW::Logging::IOReturnName(receive), cleanup, ASFW::Logging::IOReturnName(cleanup), result, ASFW::Logging::IOReturnName(result));
        }
        return result;
    }

    const auto stopPolicy = family.GetStopPolicy();
    IOReturn result = kIOReturnSuccess;
    if (stopPolicy.stopHostContextsBeforeDevice) {
        const IOReturn receive = trace("host-rx", [&] { return host_.StopPreparedReceive(); });
        const IOReturn transmit = trace("host-tx", [&] { return host_.StopPreparedTransmit(); });
        if (receive != kIOReturnSuccess) result = receive;
        if (transmit != kIOReturnSuccess && result == kIOReturnSuccess) result = transmit;
        const IOReturn device = trace("device", [&] { return family.Stop(); });
        if (device == kIOReturnAborted && TeardownRequested()) {
            RecordTeardownAbort("DeviceStop", guid);
            return kIOReturnAborted;
        }
        if (device != kIOReturnSuccess && device != kIOReturnUnsupported && result == kIOReturnSuccess) {
            result = device;
        }
        // A current device that refused its stop may still be transmitting.
        // Keep host-owned IRM reservations attached until a later stop retry;
        // releasing them could hand a live channel to another node. A stale
        // route/reset may release through the reset cleanup path instead.
        if (device != kIOReturnSuccess && device != kIOReturnUnsupported &&
            registry_.IsCurrent(policy->route)) {
            return result;
        }
        const IOReturn cleanup = registry_.IsCurrent(policy->route)
                                     ? trace("host-cleanup-irm", [&] { return host_.StopAll(); })
                                     : trace("host-cleanup-reset", [&] { return host_.StopAllAfterBusReset(); });
        if (cleanup != kIOReturnSuccess && result == kIOReturnSuccess) result = cleanup;
        return result;
    }
    // Existing families retain their established teardown ordering.
    result = trace("host-cleanup-irm", [&] { return host_.StopAll(); });
    const IOReturn device = trace("device", [&] { return family.Stop(); });
    if (device == kIOReturnAborted && TeardownRequested()) {
        RecordTeardownAbort("DeviceStop", guid);
        return kIOReturnAborted;
    }
    if (device != kIOReturnSuccess && device != kIOReturnUnsupported && result == kIOReturnSuccess) {
        result = device;
    }
    return result;
}

IOReturn StopRoutine::Rollback(uint64_t guid,
                               const Discovery::DeviceRecord& record,
                               const Discovery::DeviceRouteToken& route,
                               FamilyDriver& family,
                               const AudioStreamRuntimeCaps& caps,
                               const AudioDuplexChannels& channels) noexcept {
    if (registry_.IsCurrent(route) && !family.GetStopPolicy().stopHostContextsBeforeDevice) {
        (void)family.BreakConnections();
    }
    return Run(guid, record, family, caps, channels);
}

} // namespace ASFW::Audio::Session
