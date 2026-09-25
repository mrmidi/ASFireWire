// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "StopRoutine.hpp"

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
        return host_.StopAll();
    }

    if (profile.stopOrder.disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive) {
        // AV/C: break each PCR connection before stopping the host context that
        // fed it. The disconnect statuses are advisory; the host stops decide.
        (void)family.DisconnectPlayback();
        const kern_return_t transmit = host_.StopPreparedTransmit();
        (void)family.DisconnectCapture();
        const kern_return_t receive = host_.StopPreparedReceive();
        // The contexts are already stopped; StopAll releases the reservation and
        // the active GUID without another wire action.
        const kern_return_t cleanup = host_.StopAll();
        const IOReturn result = transmit != kIOReturnSuccess  ? transmit
                                : receive != kIOReturnSuccess ? receive
                                                              : cleanup;
        if (result != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio,
                           "[Session] stop failed GUID=0x%016llx tx=0x%08x rx=0x%08x "
                           "cleanup=0x%08x -> 0x%08x",
                           guid, transmit, receive, cleanup, result);
        }
        return result;
    }

    IOReturn result = host_.StopAll();
    const IOReturn device = family.Stop();
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
    if (registry_.IsCurrent(route)) {
        (void)family.BreakConnections();
    }
    return Run(guid, record, family, caps, channels);
}

} // namespace ASFW::Audio::Session
