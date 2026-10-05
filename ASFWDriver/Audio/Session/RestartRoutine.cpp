#include "../Model/DiscoveredRuntimeCaps.hpp"
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The sequence is the one the deleted AudioDuplexCoordinator ran
// (DuplexStartTransaction::Run), step for step; the session golden traces in
// tests/golden/session/ recorded it before the rewrite and pin it now. Device stream programming before GLOBAL_ENABLE is
// cross-validated with Linux dice-stream.c:326-374 and dice-interface.h:120-125.

#include "RestartRoutine.hpp"
#include "SessionClock.hpp"

#include "../Protocols/Backends/DirectRxFormatDescriptor.hpp"
#include "../Protocols/Backends/DuplexStreamProfile.hpp"
#include "../../Bus/IRM/IRMTypes.hpp"
#include "../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>

#include <algorithm>

namespace ASFW::Audio::Session {

namespace {

using Backends::DuplexCaptureStreamGeometry;
using Backends::DuplexHostDirection;
using Backends::DuplexPlaybackStreamGeometry;
using Backends::DuplexStreamProfile;
using Backends::DuplexStreamProfileResolver;

// A refused reservation is one line carrying the whole arithmetic: NoResources
// alone cannot tell a full bus from a planner that asked for the wrong thing.
void LogReservationRefusal(const char* direction, uint32_t streamIndex, uint64_t guid,
                           FW::Generation generation, uint32_t am824Slots,
                           uint64_t allowedChannels,
                           const Backends::IRMReservationResult& reservation) noexcept {
    ASFW_LOG_ERROR(Audio,
                   "IRM reserve REFUSED %{public}s[%u] cause=%{public}s status=0x%08x "
                   "slots=%u packet=%u overhead=%u total=%u available=%u gap=%u "
                   "allowed=0x%016llx refused=0x%016llx guid=0x%016llx gen=%u",
                   direction, streamIndex, Backends::ToString(reservation.failure),
                   reservation.status, am824Slots, reservation.charge.packetUnits,
                   reservation.charge.overheadUnits, reservation.charge.Total(),
                   reservation.availableUnits, reservation.charge.gapCount, allowedChannels,
                   reservation.refusedChannels, guid, generation.value);
}

// Isochronous transmit is unacknowledged, so a start that succeeds and carries
// no audio explains nothing; the first question is which speed the packets went
// out at. One line per start, on the control path.
void LogReservationSummary(uint64_t guid, FW::Generation generation, FW::FwSpeed linkSpeed,
                           uint8_t gapCount, uint32_t playbackStreams, uint32_t captureStreams,
                           uint32_t reservedUnits) noexcept {
    ASFW_LOG(Audio,
             "IRM reserved speed=S%u gap=%u playback=%u capture=%u total=%u/%u units "
             "guid=0x%016llx gen=%u",
             100u << static_cast<uint8_t>(linkSpeed), gapCount, playbackStreams, captureStreams,
             reservedUnits, static_cast<uint32_t>(IRM::kMaxBandwidthUnitsS400), guid,
             generation.value);
}

// A device the catalog pins to one start rate runs at that rate, whatever the
// session asked for. A device whose geometry was observed at one rate runs at
// the rate discovery observed.
[[nodiscard]] AudioClockConfig EffectiveStartClock(
    const Discovery::DeviceRecord& record, const AudioClockConfig& requested,
    const std::optional<Model::ASFWAudioDevice>& discoveredConfig) noexcept {
    if (discoveredConfig && !discoveredConfig->rateFormationCandidates.empty() &&
        std::ranges::find(discoveredConfig->sampleRates, requested.sampleRateHz) != discoveredConfig->sampleRates.end())
        return requested;
    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(record);
    if (policy != nullptr && policy->plan.streamTraits.start.startRatePinHz != 0) {
        return AudioClockConfig{.sampleRateHz = policy->plan.streamTraits.start.startRatePinHz};
    }
    if (policy != nullptr && policy->plan.streamTraits.start.startAtObservedRate &&
        discoveredConfig && discoveredConfig->sampleRates.size() == 1 && discoveredConfig->currentSampleRate != 0) {
        return AudioClockConfig{.sampleRateHz = discoveredConfig->currentSampleRate};
    }
    return requested;
}

// The discovered single-stream geometry, in the form a family serves as caps.
[[nodiscard]] uint8_t ReadLocalSid(Driver::HardwareInterface& hw) noexcept {
    return static_cast<uint8_t>(hw.ReadNodeID() & 0x3Fu);
}

[[nodiscard]] DirectRxFormatDescriptor CaptureFormat(const DuplexStreamProfile& profile,
                                                     const DuplexCaptureStreamGeometry& stream) noexcept {
    return DirectRxFormatDescriptor{
        .wireFormat = profile.captureWireFormat,
        .framing = profile.capturePacketFraming,
        .am824Slots = stream.am824Slots,
        .streamChannels = stream.pcmChannels,
        .trustConfiguredStride = profile.captureTrustConfiguredStride,
        .motuPcmChunks = profile.captureMotuPcmChunks,
        .motuPorts = profile.captureMotuPorts,
        .captureChannelMap = profile.captureChannelMap,
    };
}

} // namespace

bool RestartRoutine::TeardownRequested() const noexcept {
    return deps_.teardown != nullptr && deps_.teardown->load(std::memory_order_acquire);
}

void RestartRoutine::RecordTeardownAbort(const char* stage, uint64_t guid) noexcept {
    deps_.teardownAborts.fetch_add(1, std::memory_order_acq_rel);
    ASFW_LOG(Audio, "[Session] start aborted by teardown stage=%{public}s GUID=%llx kr=0x%x",
             stage, guid, kIOReturnAborted);
}

std::expected<RunningSession, RestartFailure> RestartRoutine::Run(const Request& request) noexcept {
    const uint64_t guid = request.guid;
    FamilyDriver& family = *request.family;
    Discovery::DeviceRecord record = request.record;
    const AudioClockConfig clock = EffectiveStartClock(record, request.clock, request.discoveredConfig);
    auto& host = deps_.host;

    // What a failure leaves behind, and how to report it.
    AudioDuplexChannels channels{};
    AudioStreamRuntimeCaps caps{};
    auto refused = [&](IOReturn status, const char* step) {
        return std::unexpected(RestartFailure{.status = status, .step = step, .stopped = true,
                                              .channels = channels, .runtimeCaps = caps});
    };
    auto aborted = [&](const char* step) {
        RecordTeardownAbort(step, guid);
        return std::unexpected(RestartFailure{.status = kIOReturnAborted, .step = step,
                                              .stopped = false, .channels = channels,
                                              .runtimeCaps = caps});
    };

    if (TeardownRequested()) {
        return aborted("Start");
    }
    const auto route = deps_.registry.CurrentRoute(record.guid);
    if (!route.has_value() || !deps_.registry.IsCurrent(*route)) {
        return refused(kIOReturnNotReady, "Route");
    }
    const auto* policy = DeviceProfiles::Audio::CurrentAudioPolicy(record);
    if (policy == nullptr || policy->route != *route ||
        !DeviceProfiles::Audio::AllowsAudioRuntime(policy->plan.support)) {
        ASFW_LOG(Audio,
                 "[Session] no current supported audio policy; refusing start GUID=0x%016llx", guid);
        return refused(kIOReturnNotReady, "Policy");
    }
    const FW::Generation generation = route->generation;

    // A failed or superseded step rolls back through the stop routine, except
    // under teardown, when nothing may touch the hardware.
    auto rollback = [&](IOReturn status, const char* step) {
        if (status == kIOReturnAborted && TeardownRequested()) {
            return std::unexpected(RestartFailure{.status = status, .step = step, .stopped = false,
                                                  .channels = channels, .runtimeCaps = caps});
        }
        const IOReturn stopStatus =
            deps_.stop.Rollback(guid, record, *route, family, caps, channels);
        return std::unexpected(RestartFailure{.status = status, .step = step,
                                              .stopped = stopStatus == kIOReturnSuccess,
                                              .channels = channels, .runtimeCaps = caps});
    };
    // The session stopped wanting this start (stop requested, device retired or
    // rebound): roll back and report Aborted, or the stop's own failure.
    auto superseded = [&](const char* step) -> std::expected<RunningSession, RestartFailure> {
        if (TeardownRequested()) {
            return aborted(step);
        }
        const IOReturn stopStatus =
            deps_.stop.Rollback(guid, record, *route, family, caps, channels);
        return std::unexpected(RestartFailure{
            .status = stopStatus == kIOReturnSuccess ? kIOReturnAborted : stopStatus,
            .step = step, .stopped = stopStatus == kIOReturnSuccess, .channels = channels,
            .runtimeCaps = caps});
    };
    auto stillWanted = [&]() noexcept {
        return !(request.superseded && request.superseded()) && deps_.registry.IsCurrent(*route);
    };

    // Offer the discovered geometry; a family that reads its own ignores it,
    // and the graph check below still requires the two to agree.
    if (request.discoveredConfig) {
        family.AdoptDiscoveredRates(request.discoveredConfig->sampleRates);
        family.AdoptDiscoveredFormations(request.discoveredConfig->rateFormationCandidates);
        if (const auto discovered = DiscoveredCaps(*request.discoveredConfig)) {
            family.AdoptDiscoveredGeometry(*discovered);
        }
    }

    // Read the stream geometry first so channel planning and the IRM see every
    // stream. Not fatal: Configure surfaces a real device error. Cross-validated
    // with FFADO dice_avdevice.cpp prepare() (m_nb_rx/m_nb_tx).
    if (const IOReturn geometry = family.LoadGeometry(); geometry != kIOReturnSuccess) {
        ASFW_LOG(Audio, "[Session] stream-geometry read failed (0x%x); planning with known caps",
                 geometry);
    }

    // The first section read may have verified a lower link speed than ROM
    // discovery; plan with the refreshed record, on the same route.
    const auto refreshed = deps_.registry.SnapshotByGuid(record.guid);
    if (!refreshed.has_value() || !deps_.registry.IsCurrent(*route)) {
        return refused(kIOReturnOffline, "Geometry");
    }
    record = *refreshed;

    const auto applyGraph = [&](AudioStreamRuntimeCaps& geometry) {
        if (!request.discoveredConfig) return true;
        const auto& config = *request.discoveredConfig;
        if (config.playbackStreams.size() != 1 || config.captureStreams.size() != 1 ||
            std::find(config.sampleRates.begin(), config.sampleRates.end(), clock.sampleRateHz) == config.sampleRates.end()) return false;
        const auto& playback = config.playbackStreams.front();
        const auto& capture = config.captureStreams.front();
        // Device stages must agree with the graph before any host DMA is armed.
        if (geometry.hostInputPcmChannels != capture.pcmChannels ||
            geometry.hostOutputPcmChannels != playback.pcmChannels ||
            geometry.deviceToHostAm824Slots != capture.am824Slots ||
            geometry.hostToDeviceAm824Slots != playback.am824Slots) return false;
        geometry.deviceToHostStreamCount = geometry.hostToDeviceStreamCount = 1;
        geometry.deviceToHostStreams[0] = {.pcmChannels = static_cast<uint16_t>(capture.pcmChannels),
            .am824Slots = static_cast<uint16_t>(capture.am824Slots)};
        geometry.hostToDeviceStreams[0] = {.pcmChannels = static_cast<uint16_t>(playback.pcmChannels),
            .am824Slots = static_cast<uint16_t>(playback.am824Slots)};
        return true;
    };
    auto initialCaps = family.RuntimeCaps().value_or(AudioStreamRuntimeCaps{});
    if (!applyGraph(initialCaps)) {
        ASFW_LOG_ERROR(Audio, "[AvcGraphBind] guid=%llx refused reason=device-geometry-or-rate", guid);
        return refused(kIOReturnUnsupported, "GraphGeometry");
    }
    DuplexStreamProfile profile =
        DuplexStreamProfileResolver::Resolve(record, initialCaps);
    if (!profile.policyResolved) {
        return refused(kIOReturnNotReady, "Profile");
    }
    channels = profile.channels;

    if (request.irm == nullptr) {
        ASFW_LOG_ERROR(Audio, "[Session] device control has no IRM client GUID=%llx", guid);
        return refused(kIOReturnNotReady, "IrmClient");
    }
    if (request.binding == nullptr) {
        return refused(kIOReturnNotReady, "BindingSource");
    }
    ASFW_LOG(Audio, "[Session] using isoch channels d2h=%u h2d=%u GUID=%llx",
             channels.deviceToHostIsoChannel, channels.hostToDeviceIsoChannel, guid);

    if (TeardownRequested()) {
        return aborted("BeginSplitDuplex");
    }
    if (const kern_return_t claim = host.BeginSplitDuplex(guid); claim != kIOReturnSuccess) {
        return refused(claim, "BeginSplitDuplex");
    }

    // 1. Claim the device and bring its clock to the target.
    if (TeardownRequested()) {
        return aborted("Configure");
    }
    const auto prepared = family.Configure(channels, clock);
    if (!prepared) {
        return rollback(prepared.error(), "Configure");
    }
    if (!stillWanted()) {
        return superseded("Configure");
    }
    caps = prepared->runtimeCaps;
    if (!applyGraph(caps)) return rollback(kIOReturnUnsupported, "GraphGeometry");
    profile = DuplexStreamProfileResolver::Resolve(record, caps, channels);
    if (!profile.policyResolved) {
        return superseded("Configure");
    }
    if (const auto delay = family.PostEnableDelayMs(); delay.has_value()) {
        profile.startOrder.postDeviceEnableDelayMs = *delay;
    }

    // 2. The IRM assigns a channel and bandwidth to every playback stream, then
    //    every capture stream. CMP families accept any channel the IRM picks;
    //    DICE profiles pass a one-bit mask.
    if (TeardownRequested()) {
        return aborted("ReservePlayback");
    }
    uint32_t reservedUnits = 0;
    uint8_t gapCount = 63;
    const FamilyDriver::ResourcePolicy resourcePolicy = family.GetResourcePolicy();
    for (uint32_t i = 0; i < channels.playbackStreamCount; ++i) {
        const DuplexPlaybackStreamGeometry& geometry = profile.playbackStreams[i];
        if (!resourcePolicy.playback.reserveHostResources) {
            continue;
        }
        const uint64_t allowed = geometry.allowedIsoChannels & resourcePolicy.playback.allowedIsoChannels;
        Backends::IRMReservationResult reservation{};
        const kern_return_t status = host.ReservePlaybackResources(
            guid, *request.irm, allowed, geometry.packetBandwidthUnits,
            reservation);
        if (status != kIOReturnSuccess) {
            LogReservationRefusal("playback", i, guid, generation, geometry.am824Slots,
                                  allowed, reservation);
            return rollback(status, "ReservePlayback");
        }
        reservedUnits += reservation.charge.Total();
        gapCount = reservation.charge.gapCount;
        channels.playbackIsoChannels[i] = reservation.channel;
        if (i == 0) {
            channels.hostToDeviceIsoChannel = reservation.channel;
        }
        if (!stillWanted()) {
            return superseded("ReservePlayback");
        }
    }
    if (TeardownRequested()) {
        return aborted("ReserveCapture");
    }
    for (uint32_t i = 0; i < channels.captureStreamCount; ++i) {
        const DuplexCaptureStreamGeometry& geometry = profile.captureStreams[i];
        if (!resourcePolicy.capture.reserveHostResources) {
            continue;
        }
        const uint64_t allowed = geometry.allowedIsoChannels & resourcePolicy.capture.allowedIsoChannels;
        Backends::IRMReservationResult reservation{};
        const kern_return_t status = host.ReserveCaptureResources(
            guid, *request.irm, allowed, geometry.packetBandwidthUnits,
            reservation);
        if (status != kIOReturnSuccess) {
            LogReservationRefusal("capture", i, guid, generation, geometry.am824Slots,
                                  allowed, reservation);
            return rollback(status, "ReserveCapture");
        }
        reservedUnits += reservation.charge.Total();
        gapCount = reservation.charge.gapCount;
        channels.captureIsoChannels[i] = reservation.channel;
        if (i == 0) {
            channels.deviceToHostIsoChannel = reservation.channel;
        }
        if (!stillWanted()) {
            return superseded("ReserveCapture");
        }
    }
    LogReservationSummary(guid, generation, profile.linkSpeed, gapCount,
                          resourcePolicy.playback.reserveHostResources ? channels.playbackStreamCount : 0,
                          resourcePolicy.capture.reserveHostResources ? channels.captureStreamCount : 0,
                          reservedUnits);

    // 3. The family writes the assigned channels to the device (CMP: into the
    //    remote PCRs) before either direction is armed, and the host DMA uses
    //    exactly the same values: allocation -> PCR -> DMA, as the Linux
    //    OXFW/CMP and FFADO lifecycles do.
    const auto assigned = family.AssignChannels(channels);
    if (!assigned) {
        return rollback(assigned.error(), "AssignChannels");
    }
    channels = *assigned;
    if (!stillWanted()) {
        return superseded("AssignChannels");
    }
    profile = DuplexStreamProfileResolver::Resolve(record, caps, channels);
    if (!profile.policyResolved) {
        return superseded("AssignChannels");
    }
    if (const auto delay = family.PostEnableDelayMs(); delay.has_value()) {
        profile.startOrder.postDeviceEnableDelayMs = *delay;
    }
    ASFW_LOG(Audio,
             "AUDIO DUPLEX START guid=0x%016llx ir=%u it=%u inCh=%u outCh=%u inSlots=%u outSlots=%u "
             "mode=blocking rxFmt=%u txFmt=%u",
             guid, channels.deviceToHostIsoChannel, channels.hostToDeviceIsoChannel,
             caps.hostInputPcmChannels, caps.hostOutputPcmChannels, caps.deviceToHostAm824Slots,
             caps.hostToDeviceAm824Slots, static_cast<uint32_t>(profile.captureWireFormat),
             static_cast<uint32_t>(profile.playbackWireFormat));

    if (request.discoveredConfig) {
        const auto& config = *request.discoveredConfig;
        const auto& capture = config.captureStreams.front();
        if (!capture.pcmSlotMap.FitsWithin(capture.pcmChannels, capture.am824Slots))
            return rollback(kIOReturnUnsupported, "GraphSlotMap");
        // Profile-owned special firmware keeps its vendor permutation and
        // input-delay policy; it never supplied a discovery slot map.
        if (config.graphResolved)
            static_cast<Wire::PcmSlotMap&>(profile.captureChannelMap) = capture.pcmSlotMap;
        profile.playbackChannelMap = config.playbackStreams.front().pcmSlotMap;
        ASFW_LOG(Audio, "[AvcGraphBind] guid=%llx rate=%u playbackPcm=%u capturePcm=%u captureMapSlots=%u",
            guid, clock.sampleRateHz, config.outputChannelCount, config.inputChannelCount,
            capture.pcmSlotMap.slotCount);
    }

    // 4. Prepare every host DMA program while the device is still disabled, in
    //    the recipe's order. The master capture stream owns clock, ZTS and
    //    replay; secondaries write their PCM slice at a running offset.
    for (const DuplexHostDirection direction : profile.startOrder.prepareOrder) {
        if (direction == DuplexHostDirection::kReceive) {
            if (TeardownRequested()) {
                return aborted("PrepareReceive");
            }
            const kern_return_t status =
                host.PrepareReceive(channels.CaptureChannel(0), deps_.hardware, request.binding,
                                    CaptureFormat(profile, profile.captureStreams[0]));
            if (status != kIOReturnSuccess) {
                return rollback(status, "PrepareReceive");
            }
            if (!stillWanted()) {
                return superseded("PrepareReceive");
            }
            for (uint32_t i = 1; i < channels.captureStreamCount; ++i) {
                const DuplexCaptureStreamGeometry& stream = profile.captureStreams[i];
                if (TeardownRequested()) {
                    return aborted("PrepareReceiveStream");
                }
                const kern_return_t streamStatus = host.PrepareReceiveStream(
                    i, channels.CaptureChannel(i), deps_.hardware, request.binding,
                    stream.pcmChannelOffset, CaptureFormat(profile, stream));
                if (streamStatus != kIOReturnSuccess) {
                    return rollback(streamStatus, "PrepareReceive");
                }
                if (!stillWanted()) {
                    return superseded("PrepareReceive");
                }
            }
            continue;
        }

        if (TeardownRequested()) {
            return aborted("PrepareTransmit");
        }
        const kern_return_t status = host.PrepareTransmit(
            channels.hostToDeviceIsoChannel, deps_.hardware, ReadLocalSid(deps_.hardware),
            profile.linkSpeed);
        if (status != kIOReturnSuccess) {
            return rollback(status, "PrepareTransmit");
        }
        if (!stillWanted()) {
            return superseded("PrepareTransmit");
        }
        // Each secondary playback stream transmits on the channel feeding the
        // device's matching RX stream.
        for (uint32_t i = 1; i < channels.playbackStreamCount; ++i) {
            if (TeardownRequested()) {
                return aborted("PrepareTransmitStream");
            }
            const kern_return_t streamStatus = host.PrepareTransmitStream(
                i, channels.PlaybackChannel(i), deps_.hardware, ReadLocalSid(deps_.hardware),
                profile.linkSpeed);
            if (streamStatus != kIOReturnSuccess) {
                return rollback(streamStatus, "PrepareTransmit");
            }
            if (!stillWanted()) {
                return superseded("PrepareTransmit");
            }
        }
    }

    // 5. A device that reports its clock state waits for a stable lock before
    //    any stream starts. BeBoB's internal clock has no meaningful state
    //    before its connections exist (bebob_stream.c:593-674); its readiness is
    //    checked after start.
    if (profile.startOrder.requiresPreStreamClockLock) {
        if (TeardownRequested()) {
            return aborted("AwaitClock");
        }
        const IOReturn status = AwaitStableClock(family, generation, clock, guid);
        if (status != kIOReturnSuccess) {
            return rollback(status, "AwaitClock");
        }
        if (!stillWanted()) {
            return superseded("AwaitClock");
        }
    }

    bool receiveStarted = false;
    bool transmitStarted = false;
    auto startReceive = [&](const char* step) -> std::optional<std::expected<RunningSession, RestartFailure>> {
        if (TeardownRequested()) {
            return aborted(step);
        }
        const kern_return_t status = host.StartPreparedReceive();
        if (status != kIOReturnSuccess) {
            return rollback(status, "StartReceive");
        }
        if (!stillWanted()) {
            return superseded("StartReceive");
        }
        receiveStarted = true;
        return std::nullopt;
    };
    auto startTransmit = [&](const char* step) -> std::optional<std::expected<RunningSession, RestartFailure>> {
        if (TeardownRequested()) {
            return aborted(step);
        }
        const kern_return_t status = host.StartPreparedTransmit();
        if (status != kIOReturnSuccess) {
            return rollback(status, "StartTransmit");
        }
        if (!stillWanted()) {
            return superseded("StartTransmit");
        }
        transmitStarted = true;
        return std::nullopt;
    };

    // 6. Arm the device. AV/C recipes keep their historical interleave, with
    //    each host context running before its PCR connection exists.
    if (profile.startOrder.startReceiveBeforeDeviceRx) {
        if (auto failed = startReceive("StartReceiveBeforeDeviceRx")) {
            return *failed;
        }
    }
    if (TeardownRequested()) {
        return aborted("ArmDeviceRx");
    }
    const auto armedRx = family.ArmDeviceRx();
    if (!armedRx) {
        return rollback(armedRx.error(), "ArmDeviceRx");
    }
    if (!stillWanted()) {
        return superseded("ArmDeviceRx");
    }
    if (armedRx->runtimeCaps.hostOutputPcmChannels != 0) {
        caps = armedRx->runtimeCaps;
    }

    if (profile.startOrder.startTransmitBeforeDeviceTx) {
        if (auto failed = startTransmit("StartTransmitBeforeDeviceTx")) {
            return *failed;
        }
    }
    if (TeardownRequested()) {
        return aborted("ArmDeviceTx");
    }
    const auto armedTx = family.ArmDeviceTxAndEnable();
    if (!armedTx) {
        return rollback(armedTx.error(), "ArmDeviceTx");
    }
    if (!stillWanted()) {
        return superseded("ArmDeviceTx");
    }
    if (armedTx->runtimeCaps.hostOutputPcmChannels != 0) {
        caps = armedTx->runtimeCaps;
    }

    // 7. Start the host contexts the recipe has not started yet, after the
    //    device's settling interval. A recipe may start receive first so the
    //    device already transmits before host transmit begins.
    IOSleep(profile.startOrder.postDeviceEnableDelayMs);
    if (TeardownRequested()) {
        return aborted("PostEnableDelay");
    }
    for (const DuplexHostDirection direction : profile.startOrder.startOrder) {
        if (direction == DuplexHostDirection::kReceive) {
            if (!receiveStarted) {
                if (auto failed = startReceive("StartReceive")) {
                    return *failed;
                }
            }
        } else if (!transmitStarted) {
            if (auto failed = startTransmit("StartTransmit")) {
                return *failed;
            }
        }
    }

    // 8. Confirm the device runs what was armed.
    if (TeardownRequested()) {
        return aborted("Confirm");
    }
    const auto confirmed = family.Confirm();
    if (!confirmed) {
        return rollback(confirmed.error(), "Confirm");
    }
    if (!stillWanted()) {
        return superseded("Confirm");
    }

    return RunningSession{
        .route = *route,
        .generation = confirmed->generation,
        .channels = channels,
        .appliedClock = confirmed->appliedClock,
        .runtimeCaps = confirmed->runtimeCaps,
    };
}

IOReturn RestartRoutine::AwaitStableClock(FamilyDriver& family, FW::Generation generation,
                                          const AudioClockConfig& clock, uint64_t guid) noexcept {
    uint32_t lockedReads = 0;
    const uint64_t deadlineMs = UptimeMilliseconds() + kClockLockTimeoutMs;

    while (UptimeMilliseconds() < deadlineMs) {
        if (TeardownRequested()) {
            RecordTeardownAbort("AwaitClock", guid);
            return kIOReturnAborted;
        }
        const uint32_t remainingMs = static_cast<uint32_t>(deadlineMs - UptimeMilliseconds());
        const auto health = family.ReadHealth(std::max(remainingMs, 1U));
        if (!health) {
            ASFW_LOG_ERROR(Audio, "Device clock health read failed before isoch start kr=0x%x",
                           health.error());
            return health.error();
        }
        if (health->generation != generation) {
            ASFW_LOG_ERROR(Audio,
                           "Device clock generation changed before isoch start expected=%u actual=%u",
                           generation.value, health->generation.value);
            return kIOReturnOffline;
        }

        const bool lockedAtTarget = health->sourceLocked &&
                                    health->nominalRateHz == clock.sampleRateHz &&
                                    health->appliedClock.sampleRateHz == clock.sampleRateHz;
        lockedReads = lockedAtTarget ? lockedReads + 1 : 0;
        if (lockedReads >= kClockStableReads) {
            ASFW_LOG(Audio, "Device clock stable before isoch start rate=%u status=0x%08x reads=%u",
                     clock.sampleRateHz, health->status, lockedReads);
            return kIOReturnSuccess;
        }

        const uint64_t afterReadMs = UptimeMilliseconds();
        if (afterReadMs >= deadlineMs) {
            break;
        }
        if (TeardownRequested()) {
            RecordTeardownAbort("AwaitClock", guid);
            return kIOReturnAborted;
        }
        IOSleep(std::min<uint64_t>(kClockLockPollMs, deadlineMs - afterReadMs));
    }

    ASFW_LOG_ERROR(Audio, "Device clock failed to stabilize before isoch start rate=%u timeoutMs=%u",
                   clock.sampleRateHz, kClockLockTimeoutMs);
    return kIOReturnTimeout;
}

} // namespace ASFW::Audio::Session
