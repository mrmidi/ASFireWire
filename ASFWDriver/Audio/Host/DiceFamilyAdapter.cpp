// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceFamilyAdapter.cpp - see DiceFamilyAdapter.hpp. Describe is
// DiceAudioBackend::EnsureNubForGuid's body (E5); the admission, route and
// publish/refresh steps around it are the host's.

#include "DiceFamilyAdapter.hpp"

#include "../Model/RateConfiguration.hpp"
#include "../Protocols/Backends/DiceRuntimeDeviceConfig.hpp"
#include "../Protocols/DICE/Core/DICETypes.hpp"
#include "../Protocols/DICE/Core/DiceNotificationRouter.hpp"
#include "../Protocols/DICE/Core/DiceRateFormats.hpp"
#include "../Protocols/DeviceProtocolChoice.hpp"
#include "../Protocols/IDeviceProtocol.hpp"
#include "../Protocols/StreamGeometryResolver.hpp"
#include "../Runtime/RateValidation.hpp"
#include "../DriverKit/Config/AudioDriverConfig.hpp"
#include "../DriverKit/Config/AudioProfileRegistry.hpp"
#include "../DriverKit/Config/DICE/DiceProfile.hpp"
#include "../../Logging/Logging.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ASFW::Audio::Host {

namespace {

// The device's own DICE registers describe its streams, and every consumer
// reads that one description: bandwidth (DuplexStreamProfile), capture framing
// (IsochDuplexHostTransport) and playback framing (the nub's per-stream arrays,
// BuildResolvedTxStreamConfig). The profile states no geometry
// (DICE_TCAT_ARCHITECTURE.md §4.2 stage C). What is left to check here: the one
// stream count a reference stack says a device overstates, and that the device
// described something usable. See StreamGeometryResolver.hpp.

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

/// Playback streams ASFWAudioDevice::StartIO can allocate: one primary plus one
/// secondary. Kept here as well so publication and start refuse the same
/// devices; StartIO carries the matching bound.
inline constexpr uint32_t kMaxPlaybackStreamsSupported = 2;

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

// The profile states no per-stream geometry.
[[nodiscard]] constexpr WireStreamGeometry NoProfileGeometry(uint32_t) noexcept {
    return {};
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

// Both directions, resolved once.
[[nodiscard]] ResolvedDeviceGeometry ResolveDeviceStreamGeometry(
    uint64_t guid,
    const AudioStreamRuntimeCaps& caps) {
    static_assert(kMaxResolvedStreams == kMaxAudioStreamsPerDirection,
                  "resolver bound drifted from the host array bound");
    static_assert(kMaxResolvedStreams == ASFW::Isoch::Audio::kMaxConfiguredStreams,
                  "resolver bound drifted from the nub's per-stream array bound");

    ResolvedDeviceGeometry resolved{};
    resolved.capture = ResolveDirectionGeometry(
        caps.deviceToHostStreamCount, 0,
        [&caps](uint32_t i) { return CaptureGeometryFromDevice(caps, i); }, NoProfileGeometry);
    // The device's count stands in both directions, as in the TCAT kexts
    // (PopulateDeviceStruct loops the full TX_NUMBER and RX_NUMBER).
    resolved.playback = ResolveDirectionGeometry(
        caps.hostToDeviceStreamCount, 0,
        [&caps](uint32_t i) { return PlaybackGeometryFromDevice(caps, i); }, NoProfileGeometry);

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

// Per-stream wire geometry for the audio side. channelOffset is the RUNNING SUM
// of preceding stream widths, the same base the vendor drivers carry
// (AlesisFirewireAudioEngine::CreateStreams advances it by each stream's own
// count); index * width coincides only while every stream is stream 0's width.
void PublishStreams(const ResolvedDirectionGeometry& direction,
                    std::vector<Model::ASFWAudioWireStream>& out) {
    out.clear();
    uint32_t channelOffset = 0;
    for (uint32_t i = 0; i < direction.StreamCount() && i < kMaxResolvedStreams; ++i) {
        const auto& geometry = direction.streams[i].geometry;
        out.push_back(Model::ASFWAudioWireStream{
            .pcmChannels = geometry.pcmChannels,
            .am824Slots = geometry.am824Slots,
            .midiPorts = geometry.midiPorts,
            .channelOffset = channelOffset,
        });
        channelOffset += geometry.pcmChannels;
    }
}

// The description from the device's loaded caps, or the refusal that stops it.
// `dev` arrives with identity and name set; the rest comes from the device.
[[nodiscard]] DescribeResult DescribeFromCaps(uint64_t guid,
                                              const Discovery::DeviceRouteToken& route,
                                              const ASFW::Isoch::Audio::DICE::DiceProfile& profile,
                                              Model::ASFWAudioDevice dev,
                                              IDeviceProtocol& protocol) {
    AudioStreamRuntimeCaps caps{};
    if (!protocol.GetRuntimeAudioStreamCaps(caps)) {
        // Without runtime caps the resolver cannot run, so the publication
        // would carry unvalidated geometry.
        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                       "GUID=0x%016llx - runtime caps unavailable, cannot validate geometry",
                       guid);
        return DescribeRefusal{kIOReturnNotReady, DiceFamilyAdapter::kCapsUnavailable};
    }

    const auto resolvedGeometry = ResolveDeviceStreamGeometry(guid, caps);
    if (!resolvedGeometry.Usable()) {
        // Publishing anyway used to give silence with nothing attributable:
        // bandwidth reserved from one description, CIP framed from the other.
        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                       "GUID=0x%016llx - resolved stream geometry is unusable",
                       guid);
        return DescribeRefusal{kIOReturnUnsupported, DiceFamilyAdapter::kGeometryUnusable};
    }

    // The resolved totals are the authority for per-stream wire geometry. The
    // HAL-facing counts keep the protocol's visibility policy: TCAT reports zero
    // host input channels for devices like the Weiss INT202 whose capture
    // streams stay hidden from CoreAudio although the wire carries them.
    const uint32_t resolvedCapturePcm = resolvedGeometry.capture.TotalPcmChannels();
    caps.hostInputPcmChannels = (caps.hostInputPcmChannels == 0) ? 0 : resolvedCapturePcm;
    caps.hostOutputPcmChannels = resolvedGeometry.playback.TotalPcmChannels();
    caps.deviceToHostStreamCount = resolvedGeometry.capture.StreamCount();
    caps.hostToDeviceStreamCount = resolvedGeometry.playback.StreamCount();

    if (ApplyDiceRuntimeCapsToDeviceConfig(caps, dev)) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend::EnsureNubForGuid: applied runtime geometry rate=%u in=%u out=%u (GUID=0x%016llx)",
                 dev.currentSampleRate, dev.inputChannelCount, dev.outputChannelCount, guid);
    }

    PublishStreams(resolvedGeometry.playback, dev.playbackStreams);
    PublishStreams(resolvedGeometry.capture, dev.captureStreams);
    // A DICE device only reaches here with a usable verdict, so the audio side
    // must never substitute profile constants for what the device reported.
    dev.resolvedGeometryRequired = !dev.playbackStreams.empty();

    // Announce every rate the device supports, as TCAT's kexts do
    // (CLOCK_CAPABILITIES & 0x7F). A device announcing no rate this build can
    // stream cannot run at all.
    uint32_t initialRate = DICE::DiceInitialRate(caps.deviceRateMask);
    if (!initialRate && Runtime::kDiceHardwareBatch && dev.inputChannelCount != 0) {
        const auto formations = protocol.RateFormations();
        if (formations && !formations->empty()) initialRate = formations->front().sampleRateHz;
    }
    if (initialRate == 0) {
        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                       "GUID=0x%016llx - device supports no rate up to %u Hz (rates=0x%02x)",
                       guid, DICE::kDiceMaxStreamingRateHz, caps.deviceRateMask);
        return DescribeRefusal{kIOReturnUnsupported, DiceFamilyAdapter::kNoStreamableRate};
    }
    dev.sampleRates = DICE::DicePublishedRates(caps.deviceRateMask);
    dev.deviceSampleRates = true;
    dev.currentSampleRate = initialRate;
    if (const auto formations = protocol.RateFormations();
        formations && !formations->empty() && dev.inputChannelCount != 0) {
        dev.rateFormationCandidates = *formations;
        dev.usesRateFormations = true;
        dev.rateRouteIncarnation = route.deviceIncarnation;
        dev.rateRouteEpoch = route.routeEpoch;
        dev.rateBusGeneration = route.generation.value;
        dev.sampleRates.clear();
        for (const auto& formation : *formations)
            if (Runtime::RateEnabled(formation, initialRate, true))
                dev.sampleRates.push_back(formation.sampleRateHz);
        // Generic non-EAP hardware can have only its observed mode known.
        // Publish a coherent known formation, never another mode's geometry
        // under a convenient scalar clock.
        if (std::ranges::find(dev.sampleRates, initialRate) == dev.sampleRates.end()) {
            if (dev.sampleRates.empty()) {
                return DescribeRefusal{kIOReturnUnsupported, DiceFamilyAdapter::kNoEnabledFormation};
            }
            dev.currentSampleRate =
                std::ranges::find(dev.sampleRates, caps.sampleRateHz) != dev.sampleRates.end()
                    ? caps.sampleRateHz
                    : dev.sampleRates.front();
        }
        const auto selected = Model::WithRateFormation(dev, dev.currentSampleRate);
        if (!selected) {
            return DescribeRefusal{kIOReturnUnsupported, DiceFamilyAdapter::kFormationNotSelectable};
        }
        dev = *selected;
    }
    if (!dev.usesRateFormations && !DICE::DiceRateIsStreamable(caps.sampleRateHz)) {
        // Legacy endpoints without a complete catalog cannot project another
        // rate mode safely; retain the existing warning.
        ASFW_LOG_WARNING(Audio,
                         "DiceAudioBackend::EnsureNubForGuid: GUID=0x%016llx is running at "
                         "%u Hz, above the %u Hz streaming ceiling; published geometry is that "
                         "mode's and may not match %u Hz",
                         guid, caps.sampleRateHz, DICE::kDiceMaxStreamingRateHz, initialRate);
    }

    // Refuse a device this build cannot arm, at publication rather than at the
    // first StartIO: ASFWAudioDevice allocates one primary and one secondary TX
    // stream, and an endpoint that fails every start looks like a broken device
    // rather than an unsupported one. StartIO enforces the same bound.
    if (dev.playbackStreams.size() > kMaxPlaybackStreamsSupported) {
        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                       "GUID=0x%016llx - device carries %zu playback streams, this "
                       "build configures at most %u",
                       guid, dev.playbackStreams.size(), kMaxPlaybackStreamsSupported);
        return DescribeRefusal{kIOReturnUnsupported, DiceFamilyAdapter::kTooManyPlaybackStreams};
    }

    for (size_t i = 0; i < dev.playbackStreams.size(); ++i) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend::EnsureNubForGuid: playback stream %zu pcm=%u dbs=%u "
                 "midi=%u offset=%u (GUID=0x%016llx)",
                 i, dev.playbackStreams[i].pcmChannels, dev.playbackStreams[i].am824Slots,
                 dev.playbackStreams[i].midiPorts, dev.playbackStreams[i].channelOffset, guid);
    }

    // Name the device now that its geometry is known. For a range sharing one
    // identity (Midas Venice F16/F24/F32) the measured capture width is the
    // only thing that tells the variants apart.
    if (const char* resolvedName =
            profile.NameForGeometry(dev.inputChannelCount, dev.outputChannelCount)) {
        if (dev.deviceName != resolvedName) {
            ASFW_LOG(Audio,
                     "DiceAudioBackend::EnsureNubForGuid: named from geometry "
                     "'%{public}s' -> '%{public}s' in=%u out=%u (GUID=0x%016llx)",
                     dev.deviceName.c_str(), resolvedName,
                     dev.inputChannelCount, dev.outputChannelCount, guid);
            dev.deviceName = resolvedName;
        }
    }

    // The device's per-channel labels. Host input == device TX, host output ==
    // device RX (AudioTypes.hpp), which is how GetChannelLabels reports them.
    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    if ((!dev.usesRateFormations ||
         DICE::DiceRateMode(caps.sampleRateHz) == DICE::DiceRateMode(dev.currentSampleRate)) &&
        protocol.GetChannelLabels(inNames, outNames)) {
        if (!inNames.empty()) dev.inputChannelNames = std::move(inNames);
        if (!outNames.empty()) dev.outputChannelNames = std::move(outNames);
        ASFW_LOG(Audio,
                 "DiceAudioBackend::EnsureNubForGuid: applied device channel labels in=%zu out=%zu (GUID=0x%016llx)",
                 dev.inputChannelNames.size(), dev.outputChannelNames.size(), guid);
    }
    return dev;
}

} // namespace

DiceFamilyAdapter::DiceFamilyAdapter(DICE::DiceNotificationRouter& notifications) noexcept
    : notifications_(notifications) {}

DiceFamilyAdapter::~DiceFamilyAdapter() noexcept {
    notifications_.ClearObserver(this);
}

void DiceFamilyAdapter::Describe(const DescribeInput& in, DescribeDone done) {
    const uint64_t guid = in.record.guid;
    const auto& policy = *in.policy;
    const auto route = policy.route;
    const auto choice = ChooseDeviceProtocol(policy.plan);
    const uint32_t profileBuilderId = choice.has_value() ? static_cast<uint32_t>(choice->builder) : 0U;
    const auto* profile =
        ASFW::Isoch::Audio::AudioProfileRegistry::DiceProfileForBuilderId(profileBuilderId);
    if (!profile) {
        ASFW_LOG(Audio,
                 "DiceAudioBackend::EnsureNubForGuid: no isoch profile for GUID=0x%016llx "
                 "vendor=0x%06x model=0x%06x builder=%u",
                 guid, in.record.vendorId, in.record.modelId, profileBuilderId);
        done(DescribeRefusal{kIOReturnUnsupported, kNoProfile});
        return;
    }

    ASFW_LOG(Audio,
             "DiceAudioBackend::EnsureNubForGuid: matched profile=%{public}s for GUID=0x%016llx",
             profile->Name(), guid);

    Model::ASFWAudioDevice dev{};
    dev.guid = in.record.guid;
    dev.vendorId = in.record.vendorId;
    dev.modelId = in.record.modelId;
    // Carry the catalog's answer to the audio side, which only ever sees
    // scalars and must not have to re-derive it.
    dev.profileBuilderId = profileBuilderId;
    dev.deviceName = profile->Name();
    // The shared profiles carry no model name of their own; the catalog row or,
    // for an unlisted unit, the Config ROM text has it.
    if (choice.has_value() &&
        (choice->builder == DeviceProfiles::Audio::ProfileBuilderId::GenericDice ||
         choice->builder == DeviceProfiles::Audio::ProfileBuilderId::WeissDac)) {
        const auto& plan = policy.plan;
        if (!plan.modelName.empty()) {
            dev.deviceName = plan.vendorName.empty() ? plan.modelName
                                                     : plan.vendorName + " " + plan.modelName;
        }
    }
    // Channel counts and rates come from the device's caps; 48 kHz is the rate
    // bring-up programs by default.
    dev.inputPlugName = "Input";
    dev.outputPlugName = "Output";
    dev.currentSampleRate = 48000u;

    // A live catalog endpoint changes only inside its host configuration
    // window: a republish must not reset the formation a rate transaction
    // committed (§4.3).
    const bool keepCommitted = in.committed.has_value() && in.committed->usesRateFormations;
    auto protocol = in.protocol;

    auto finish = [guid, route, profile, keepCommitted, done](
                      Model::ASFWAudioDevice dev, const std::shared_ptr<IDeviceProtocol>& protocol) {
        if (keepCommitted) {
            done(KeepCommitted{});
            return;
        }
        if (!protocol) {
            // Nothing can answer for the device's geometry. The device is not
            // lost: a record update or a restart describes it again once a
            // protocol exists.
            ASFW_LOG_ERROR(Audio,
                           "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                           "GUID=0x%016llx - no protocol, geometry cannot be validated",
                           guid);
            done(DescribeRefusal{kIOReturnNotReady, kNoProtocol});
            return;
        }
        done(DescribeFromCaps(guid, route, *profile, std::move(dev), *protocol));
    };

    // Channel labels live in the TCAT stream-format name sections, cached only
    // once runtime caps load. Load them before the first publish so CoreAudio
    // shows the real names from the start. A failed load refuses here: caps
    // cached by an earlier load survive a later failure, so going on would
    // describe a stale device.
    if (protocol) {
        protocol->EnsureRuntimeStreamGeometry(
            [finish, dev, protocol, guid, done](IOReturn status) mutable {
                if (status != kIOReturnSuccess) {
                    ASFW_LOG_ERROR(Audio,
                                   "DiceAudioBackend::EnsureNubForGuid: refusing to publish "
                                   "GUID=0x%016llx - geometry load failed status=0x%08x",
                                   guid, status);
                    done(DescribeRefusal{status, kGeometryLoadFailed});
                    return;
                }
                finish(std::move(dev), protocol);
            });
        return;
    }
    finish(std::move(dev), protocol);
}

FaultVerdict DiceFamilyAdapter::JudgeRuntimeFault(uint64_t guid, DuplexRestartReason reason,
                                                 FaultContext& context) {
    (void)guid;
    switch (reason) {
        case DuplexRestartReason::kRecoverAfterTimingLoss:
        case DuplexRestartReason::kRecoverAfterCycleInconsistent:
        case DuplexRestartReason::kRecoverAfterLockLoss:
        case DuplexRestartReason::kRecoverAfterTxFault:
            break;
        default:
            // A config change or a reset rebind always restarts.
            return FaultVerdict::kRestart;
    }
    // Health gate: a host-side replay discontinuity (StartIO/StopIO churn, an
    // RX packet gap) fires the same detectors as a genuine device clock drop.
    // When the device still reports a locked, healthy clock, the RX epoch reset
    // re-establishes cadence and replay; a restart would only tear down a
    // healthy session. A read failure restarts: never suppress a recovery on
    // missing evidence.
    if (context.Cancelled()) return FaultVerdict::kRestart;
    const auto health = context.ReadHealth(kHealthReadTimeoutMs);
    if (health && health->sourceLocked && health->clockReferenceHealthy) {
        return FaultVerdict::kSelfHealed;
    }
    return FaultVerdict::kRestart;
}

void DiceFamilyAdapter::SetEventSink(DeviceEventSink* sink) noexcept {
    if (sink != nullptr) {
        sink_.store(sink, std::memory_order_release);
        notifications_.SetObserver(this, &DiceFamilyAdapter::NotificationThunk);
        return;
    }
    notifications_.ClearObserver(this);
    sink_.store(nullptr, std::memory_order_release);
}

void DiceFamilyAdapter::OnNotification(uint64_t guid, uint32_t bits) noexcept {
    DeviceEventSink* sink = sink_.load(std::memory_order_acquire);
    if (sink == nullptr) return;
    // The device changed its stream configuration: restart the running streams
    // on the new one (TCAT NotificationWriteCallback, AUDIO_SESSION_REDESIGN.md §2.4).
    if ((bits & (DICE::Notify::kRxConfigChange | DICE::Notify::kTxConfigChange)) != 0) {
        sink->OnDeviceEvent(guid, DeviceEvent::kStreamConfigChanged, bits);
    }
    if ((bits & (DICE::Notify::kLockChange | DICE::Notify::kExtStatus)) != 0) {
        sink->OnDeviceEvent(guid, DeviceEvent::kClockStatusChanged, bits);
    }
}

void DiceFamilyAdapter::NotificationThunk(void* context, uint64_t guid, uint32_t bits) noexcept {
    if (auto* self = static_cast<DiceFamilyAdapter*>(context)) {
        self->OnNotification(guid, bits);
    }
}

} // namespace ASFW::Audio::Host
