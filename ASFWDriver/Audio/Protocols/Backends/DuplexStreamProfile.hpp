// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexStreamProfile.hpp - Resolved stream geometry and host-start recipe for duplex audio

#pragma once

#include "../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../../DeviceProfiles/Audio/ResolvedDevicePolicy.hpp"
#include "../../../Discovery/DiscoveryTypes.hpp"
#include "../../../Bus/IRM/IRMTypes.hpp"
#include "../DeviceProtocolChoice.hpp"
#include "../../Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../../Wire/AMDTP/AmdtpTypes.hpp"
#include "../../Wire/AMDTP/PcmSlotMap.hpp"
#include "../../Wire/MOTU/MotuPortLayout.hpp"
#include "../../DriverKit/Config/MOTU/MotuV2Profile.hpp"
#include "../../Engine/Direct/Rx/RxCaptureChannelMap.hpp"
#include "../../Families/BeBoB/MAudio/MAudioCaptureChannelMap.hpp"
#include "../AudioTypes.hpp"
#include "../IDeviceProtocol.hpp"

#include <array>
#include <cstdint>

namespace ASFW::Audio::Backends {

enum class DuplexHostDirection : uint8_t {
    kReceive,
    kTransmit,
};

// The device enable happens before this recipe's startOrder. DICE requires all
// host contexts to be prepared while GLOBAL_ENABLE is clear, then starts IR
// before IT after a short post-enable settling interval.
struct DuplexStartOrderRecipe {
    // DICE programs device RX/TX before starting the corresponding host context.
    // FW-72 can select the inverse interleave for a protocol that requires it.
    bool startReceiveBeforeDeviceRx{false};
    bool startTransmitBeforeDeviceTx{false};
    // DICE reports an explicit GLOBAL clock-lock state before its stream
    // enable. BeBoB with an internal clock has no equivalent pre-CMP state;
    // validate its PCR leases and packet liveness after host RX/TX starts.
    bool requiresPreStreamClockLock{true};
    std::array<DuplexHostDirection, 2> prepareOrder{
        DuplexHostDirection::kReceive,
        DuplexHostDirection::kTransmit,
    };
    std::array<DuplexHostDirection, 2> startOrder{
        DuplexHostDirection::kReceive,
        DuplexHostDirection::kTransmit,
    };
    uint32_t postDeviceEnableDelayMs{2};
};

struct DuplexStopOrderRecipe {
    // Preserve AV/C's PCR/host-context interleave. DICE leaves this false and
    // retains its existing StopAll -> StopDuplex sequence.
    bool disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive{false};
};

// Host receive geometry for one device-to-host stream. `pcmChannels == 0`
// preserves the legacy single-stream full-width receive path.
struct DuplexCaptureStreamGeometry {
    uint8_t isoChannel{AudioStreamWireInfo::kInvalidIsoChannel};
    uint32_t pcmChannelOffset{0};
    uint32_t pcmChannels{0};
    uint32_t am824Slots{0};
    uint32_t packetBandwidthUnits{0};
    uint64_t allowedIsoChannels{0};
};

// Retained even though the current host IT seam derives its own packet geometry:
// the resolver remains the one place that owns both directions' stream facts.
struct DuplexPlaybackStreamGeometry {
    uint8_t isoChannel{AudioStreamWireInfo::kInvalidIsoChannel};
    uint32_t pcmChannels{0};
    uint32_t am824Slots{0};
    uint32_t packetBandwidthUnits{0};
    uint64_t allowedIsoChannels{0};
};

struct DuplexStreamProfile {
    // A profile without a current catalog decision is not safe to consume for
    // stream setup. Keep this explicit so stale route policy cannot silently
    // turn into generic DICE behavior.
    bool policyResolved{false};
    AudioDuplexChannels channels{};
    // The one speed for this device's isochronous streams: what the packets are
    // transmitted at and what the IRM was charged for. Apple and Linux both keep
    // a single per-device value (IOFWIsochChannel.cpp:653-664, dice-stream.c
    // allocate + amdtp_stream_start); two answers means charging for one bus and
    // transmitting on another.
    //
    // Conservative initialization; discovery supplies the resolved path speed
    // before stream planning. S100 is valid, never an "unset" sentinel -- and
    // because this value is stamped into the transmit header as well as charged
    // to the IRM, erring low only over-reserves, while erring high would
    // transmit faster than the path supports.
    FW::FwSpeed linkSpeed{FW::FwSpeed::S100};
    AudioStreamRuntimeCaps runtimeCaps{};
    std::array<DuplexCaptureStreamGeometry, kMaxAudioStreamsPerDirection> captureStreams{};
    std::array<DuplexPlaybackStreamGeometry, kMaxAudioStreamsPerDirection> playbackStreams{};
    Encoding::AudioWireFormat captureWireFormat{Encoding::AudioWireFormat::kAM824};
    Encoding::AudioWireFormat playbackWireFormat{Encoding::AudioWireFormat::kAM824};
    // Loud OXFW units stamp an unreliable dbs in device->host packets; when set,
    // the RX decode takes its stride from the configured AM824 slot count instead
    // of the CIP header (Linux snd-oxfw SND_OXFW_QUIRK_WRONG_DBS semantics).
    bool captureTrustConfiguredStride{false};
    // M-Audio special firmware advances DBC on high-rate NO-DATA packets.
    bool captureEmptyPacketHasWrongDbc{false};

    // MOTU only: PCM chunks per data block, per direction. Its samples are 3-byte chunks
    // rather than quadlet slots, so the am824Slots geometry above does not describe them
    // and the receive path needs this instead. Zero for every other family.
    uint32_t captureMotuPcmChunks{0};
    uint32_t playbackMotuPcmChunks{0};
    // MOTU only: which chunk each host input channel reads. The playback map rides the
    // audio driver's TxStreamPolicy instead, since that side encodes playback.
    Encoding::Motu::MotuPortMap captureMotuPorts{};
    DuplexStartOrderRecipe startOrder{};
    DuplexStopOrderRecipe stopOrder{};
    AudioEngine::Direct::Rx::RxCaptureChannelMap captureChannelMap{};
    Wire::PcmSlotMap playbackChannelMap{};
};

// The coordinator deliberately delegates all device identity checks and stream
// geometry policy to this resolver. It owns fixed-vs-dynamic channel policy,
// device quirks, AM824 geometry, resource costs, and host-start ordering as
// immutable profile data.
class DuplexStreamProfileResolver final {
  public:
    [[nodiscard]] static DuplexStreamProfile Resolve(const Discovery::DeviceRecord& record,
                                                     const IDeviceProtocol* protocol) noexcept {
        AudioStreamRuntimeCaps caps{};
        const bool haveCaps = protocol != nullptr && protocol->GetRuntimeAudioStreamCaps(caps);
        return Resolve(record, haveCaps ? caps : AudioStreamRuntimeCaps{});
    }

    [[nodiscard]] static DuplexStreamProfile Resolve(const Discovery::DeviceRecord& record,
                                                     const AudioStreamRuntimeCaps& caps) noexcept {
        return Build(record, caps, ResolveChannels(caps),
                     DeviceProfiles::Audio::CurrentAudioPolicy(record));
    }

    // Device prepare can refresh stream caps after channels have already been
    // assigned and written into the pending restart session. Retain that channel
    // assignment while resolving the refreshed wire geometry.
    [[nodiscard]] static DuplexStreamProfile
    Resolve(const Discovery::DeviceRecord& record, const AudioStreamRuntimeCaps& caps,
            const AudioDuplexChannels& assignedChannels) noexcept {
        return Build(record, caps, assignedChannels,
                     DeviceProfiles::Audio::CurrentAudioPolicy(record));
    }

  private:
    static constexpr uint64_t kAllIsoChannels = ~uint64_t{0};
    static constexpr uint8_t kDefaultCaptureIsoChannel = 1;
    static constexpr uint8_t kDefaultPlaybackIsoChannel = 0;

    // The packet term. Under Apple IOFWIsochChannel wire parity
    // (IOFWIsochChannel.cpp:664), only the packet term is charged against
    // BANDWIDTH_AVAILABLE; no gap arbitration overhead is subtracted from the
    // IRM ledger.
    [[nodiscard]] static constexpr uint32_t
    AmdtpPacketBandwidthUnits(uint32_t am824Slots, uint32_t sampleRateHz,
                              FW::FwSpeed speed) noexcept {
        const auto rate = Encoding::AmdtpRateGeometryForSampleRate(
            sampleRateHz != 0 ? sampleRateHz : 48000U);
        const uint32_t blocksPerPacket = rate ? rate->sytIntervalFrames : 8U;
        const uint32_t slots = am824Slots != 0 ? am824Slots : 1U;
        const uint32_t maxPayloadBytes = 8U + blocksPerPacket * slots * 4U;
        return IRM::PacketBandwidthUnits(maxPayloadBytes, static_cast<uint8_t>(speed));
    }

    [[nodiscard]] static constexpr uint64_t FixedChannelMask(uint8_t channel) noexcept {
        return IsValidIsoChannel(channel) ? (uint64_t{1} << channel) : 0;
    }

    [[nodiscard]] static constexpr bool IsValidIsoChannel(uint8_t channel) noexcept {
        return channel <= 0x3F;
    }

    [[nodiscard]] static constexpr uint32_t ClampStreamCount(uint32_t count) noexcept {
        if (count == 0) {
            return 1;
        }
        return count > kMaxAudioStreamsPerDirection ? kMaxAudioStreamsPerDirection : count;
    }

    /// Everything below used to be nine hand-written identity predicates
    /// sitting next to the geometry they gate. They are catalog rows now: the
    /// facts are the same, but they live with the rest of what is known about
    /// the device instead of being rediscovered here.
    [[nodiscard]] static DeviceProfiles::Audio::DeviceStreamTraits
    TraitsFor(const DeviceProfiles::Audio::ResolvedDevicePolicy* policy) noexcept {
        return policy != nullptr ? policy->plan.streamTraits
                                 : DeviceProfiles::Audio::DeviceStreamTraits{};
    }

    [[nodiscard]] static AudioDuplexChannels
    ResolveChannels(const AudioStreamRuntimeCaps& caps) noexcept {
        AudioDuplexChannels channels{
            .deviceToHostIsoChannel = kDefaultCaptureIsoChannel,
            .hostToDeviceIsoChannel = kDefaultPlaybackIsoChannel,
        };

        channels.captureStreamCount = ClampStreamCount(caps.deviceToHostStreamCount);
        channels.playbackStreamCount = ClampStreamCount(caps.hostToDeviceStreamCount);

        // Stream zero retains the legacy scalar channel. Remaining streams are
        // assigned the lowest channel not already used by either direction.
        uint64_t usedChannels = 0;
        const auto markUsed = [&usedChannels](uint8_t channel) noexcept {
            if (channel <= 0x3F) {
                usedChannels |= (uint64_t{1} << channel);
            }
        };
        const auto nextFree = [&usedChannels]() noexcept -> uint8_t {
            for (uint8_t channel = 0; channel <= 0x3F; ++channel) {
                if ((usedChannels & (uint64_t{1} << channel)) == 0) {
                    usedChannels |= (uint64_t{1} << channel);
                    return channel;
                }
            }
            return AudioStreamWireInfo::kInvalidIsoChannel;
        };

        channels.captureIsoChannels[0] = IsValidIsoChannel(caps.deviceToHostIsoChannel)
                                             ? caps.deviceToHostIsoChannel
                                             : channels.deviceToHostIsoChannel;
        channels.playbackIsoChannels[0] = IsValidIsoChannel(caps.hostToDeviceIsoChannel)
                                              ? caps.hostToDeviceIsoChannel
                                              : channels.hostToDeviceIsoChannel;
        markUsed(channels.captureIsoChannels[0]);
        markUsed(channels.playbackIsoChannels[0]);

        for (uint32_t i = 1; i < channels.captureStreamCount; ++i) {
            channels.captureIsoChannels[i] = nextFree();
        }
        for (uint32_t i = 1; i < channels.playbackStreamCount; ++i) {
            channels.playbackIsoChannels[i] = nextFree();
        }

        channels.deviceToHostIsoChannel = channels.captureIsoChannels[0];
        channels.hostToDeviceIsoChannel = channels.playbackIsoChannels[0];
        return channels;
    }

    [[nodiscard]] static DuplexStreamProfile Build(const Discovery::DeviceRecord& record,
                                                   const AudioStreamRuntimeCaps& caps,
                                                   const AudioDuplexChannels& channels,
                                                   const DeviceProfiles::Audio::ResolvedDevicePolicy* policy) noexcept {
        DuplexStreamProfile profile{
            .policyResolved = policy != nullptr,
            .channels = channels,
            .linkSpeed = record.link.isochToNode,
            .runtimeCaps = caps,
        };
        const auto traits = TraitsFor(policy);
        const uint64_t allowedChannels =
            traits.cmpChoosesIsoChannel ? kAllIsoChannels : 0;

        // AM824 uses one data-block slot per PCM channel plus any MIDI slots;
        // the controller consumes the already-discovered DBS values unchanged.
        // Cross-validated with Linux sound/firewire/amdtp-am824.c:42-96.
        const bool multiCapture = channels.captureStreamCount > 1;
        uint32_t captureChannelOffset = 0;
        for (uint32_t i = 0; i < channels.captureStreamCount; ++i) {
            const AudioStreamWireInfo& stream = caps.deviceToHostStreams[i];
            DuplexCaptureStreamGeometry& geometry = profile.captureStreams[i];
            geometry.isoChannel = channels.CaptureChannel(i);
            geometry.pcmChannelOffset = captureChannelOffset;
            geometry.pcmChannels = multiCapture ? stream.pcmChannels : 0;
            geometry.am824Slots = multiCapture ? stream.am824Slots : caps.deviceToHostAm824Slots;
            geometry.packetBandwidthUnits = AmdtpPacketBandwidthUnits(
                geometry.am824Slots, caps.sampleRateHz, profile.linkSpeed);
            // CMP (including BridgeCo/BeBoB) does not own a fixed channel;
            // IRM selects one, which is then committed back to its PCR.
            geometry.allowedIsoChannels = traits.cmpChoosesIsoChannel
                                              ? allowedChannels
                                              : FixedChannelMask(geometry.isoChannel);
            captureChannelOffset += geometry.pcmChannels;
        }

        for (uint32_t i = 0; i < channels.playbackStreamCount; ++i) {
            const AudioStreamWireInfo& stream = caps.hostToDeviceStreams[i];
            DuplexPlaybackStreamGeometry& geometry = profile.playbackStreams[i];
            geometry.isoChannel = channels.PlaybackChannel(i);
            geometry.pcmChannels = stream.pcmChannels != 0
                                       ? stream.pcmChannels
                                       : (i == 0 ? caps.hostOutputPcmChannels : 0U);
            geometry.am824Slots = stream.am824Slots != 0
                                       ? stream.am824Slots
                                       : (i == 0 ? caps.hostToDeviceAm824Slots : 0U);
            geometry.packetBandwidthUnits = AmdtpPacketBandwidthUnits(
                geometry.am824Slots, caps.sampleRateHz, profile.linkSpeed);
            geometry.allowedIsoChannels = traits.cmpChoosesIsoChannel
                                              ? allowedChannels
                                              : FixedChannelMask(geometry.isoChannel);
        }

        if (policy != nullptr && policy->plan.family ==
                                     DeviceProfiles::Audio::AudioFamilyProviderId::MotuRegister) {
            // MOTU is chunk-framed in both directions. The chunk counts come from the
            // device's own registers via PrepareDuplex, which MotuV2Protocol reports as
            // runtime caps -- there is no profile table to read them from, since model_id
            // is 0.
            profile.captureWireFormat = Encoding::AudioWireFormat::kMotuV2;
            profile.playbackWireFormat = Encoding::AudioWireFormat::kMotuV2;
            profile.captureMotuPcmChunks = caps.deviceToHostPcmChunks != 0
                                               ? caps.deviceToHostPcmChunks
                                               : caps.hostInputPcmChannels;
            profile.playbackMotuPcmChunks = caps.hostToDevicePcmChunks != 0
                                                ? caps.hostToDevicePcmChunks
                                                : caps.hostOutputPcmChannels;
            // The version of the unit the catalog actually matched, not the
            // flat shim, which takes the first non-zero value across every unit
            // directory and so can name a version no single unit published.
            const auto choice = policy->plan.unitVersion;
            profile.captureMotuPorts = Isoch::Audio::MOTU::Profiles::CapturePortsForSwVersion(
                choice != 0 ? choice : record.unitSwVersion.value_or(0U));
        }

        // Runtime-conditional on purpose: this device switches wire format with
        // its configuration, so the identity grants the permission and the
        // measured geometry decides whether it applies.
        if (traits.rawPcm24In32WhenEightInNineSlots && caps.hostInputPcmChannels == 8 &&
            caps.deviceToHostAm824Slots == 9) {
            profile.captureWireFormat = Encoding::AudioWireFormat::kRawPcm24In32;
        }
        if (traits.rawPcm24In32WhenEightInNineSlots && caps.hostOutputPcmChannels == 8 &&
            caps.hostToDeviceAm824Slots == 9) {
            profile.playbackWireFormat = Encoding::AudioWireFormat::kRawPcm24In32;
        }

        if (traits.captureTrustConfiguredStride) {
            // The capture-side CIP dbs field is untrusted and the configured
            // slot count is the authority. Loud/Mackie (snd-oxfw oxfw.c:189-196;
            // amdtp-stream.c:766-769 substitutes the configured data-block
            // size), and Fireworks, which gives dbc its own meaning, whose
            // NO-DATA packets carry tag 0, and whose firmware 4.6.0 stamps a
            // wrong dbs above 88.2 kHz.
            profile.captureTrustConfiguredStride = true;
        }

        if (policy != nullptr &&
            (policy->plan.profileBuilder == DeviceProfiles::Audio::ProfileBuilderId::MAudioFireWire1814 ||
             policy->plan.profileBuilder == DeviceProfiles::Audio::ProfileBuilderId::MAudioProjectMix)) {
            profile.captureEmptyPacketHasWrongDbc = true;
        }

        // The special-firmware personas cannot answer the BridgeCo channel
        // position query. Their model-specific AM824 slot order is resolved
        // once from the catalog's profile choice and the current wire width.
        if (policy != nullptr) {
            profile.captureChannelMap =
                Families::BeBoB::MAudio::CaptureChannelMapFor(
                    policy->plan.profileBuilder, caps.hostInputPcmChannels);
        }

        using DeviceProfiles::Audio::StreamStartShape;
        switch (traits.startShape) {
        case StreamStartShape::ApogeeInterleaved:
            // Preserve the prior AVCAudioBackend ordering:
            // host IR -> CMP oPCR -> host IT -> CMP iPCR. The runner uses these
            // profile flags to interleave host starts with the neutral device
            // stages. CMP operations retain their adapter-owned 250 ms timeout.
            profile.startOrder.startReceiveBeforeDeviceRx = true;
            profile.startOrder.startTransmitBeforeDeviceTx = true;
            profile.startOrder.postDeviceEnableDelayMs = 0;
            profile.stopOrder
                .disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive = true;
            break;

        case StreamStartShape::CmpReceiveThenTransmit:
            // Linux's CMP choreography: reserve both resources, establish the
            // remote iPCR then oPCR, and only then start the AMDTP domain,
            // receive before transmit. Shared by three families that used to
            // state it three times over, identically:
            //
            //  - BeBoB. Keep the wire-visible ordering explicit rather than
            //    inheriting an incidental generic/DICE default.
            //    bebob_stream.c:525-590, 593-674.
            //  - The Oxford-run Onyx-i, which shares the BeBoB base's
            //    choreography and is SYT-unaware (snd-oxfw CIP_UNAWARE_SYT), so
            //    there is no pre-stream clock to lock against.
            //  - The Fireworks-run Onyx 400F (fireworks_stream.c:
            //    cmp_connection_establish for both plugs, then
            //    amdtp_domain_start), SYT-unaware with an internal clock the
            //    host cannot observe before the connection exists.
            //
            // The pre-stream lock gate must not run for any of them.
            // Field-verified on the 400F 2026-09-13: with the gate on, the
            // coordinator polled HWCTL GET_CLOCK ~50 times and failed the start
            // at GlobalClockLock.
            profile.startOrder.startReceiveBeforeDeviceRx = false;
            profile.startOrder.startTransmitBeforeDeviceTx = false;
            profile.startOrder.requiresPreStreamClockLock = false;
            profile.startOrder.startOrder = {
                DuplexHostDirection::kReceive,
                DuplexHostDirection::kTransmit,
            };
            profile.startOrder.postDeviceEnableDelayMs = 0;
            break;

        case StreamStartShape::TransmitFirst:
            // INT202/203 are output-only in CoreAudio but retain both DICE
            // directions on the wire. Start host IT first after GLOBAL_ENABLE
            // so its AM824 packets can establish the device receive-clock path;
            // source lock is inspected by the DICE adapter post-start, not used
            // as a coordinator admission gate. Linux's Weiss tables retain
            // two PCM channels for both DICE directions (dice-weiss.c:10-35).
            profile.startOrder.requiresPreStreamClockLock = false;
            profile.startOrder.startOrder = {
                DuplexHostDirection::kTransmit,
                DuplexHostDirection::kReceive,
            };
            break;

        case StreamStartShape::MAudioSpecial:
            profile.startOrder.requiresPreStreamClockLock = false;
            profile.startOrder.startOrder = {
                DuplexHostDirection::kTransmit,
                DuplexHostDirection::kReceive,
            };
            profile.startOrder.postDeviceEnableDelayMs = 0;
            break;

        case StreamStartShape::Default:
            break;
        }
        return profile;
    }
};

} // namespace ASFW::Audio::Backends
