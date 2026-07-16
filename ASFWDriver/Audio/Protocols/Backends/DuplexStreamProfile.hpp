// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexStreamProfile.hpp - Resolved stream geometry and host-start recipe for duplex audio

#pragma once

#include "../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "../../../Discovery/DiscoveryTypes.hpp"
#include "../../Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../../Wire/AMDTP/AmdtpTypes.hpp"
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
    uint32_t bandwidthUnits{0};
    uint64_t allowedIsoChannels{0};
};

// Retained even though the current host IT seam derives its own packet geometry:
// the resolver remains the one place that owns both directions' stream facts.
struct DuplexPlaybackStreamGeometry {
    uint8_t isoChannel{AudioStreamWireInfo::kInvalidIsoChannel};
    uint32_t pcmChannels{0};
    uint32_t am824Slots{0};
    uint32_t bandwidthUnits{0};
    uint64_t allowedIsoChannels{0};
};

struct DuplexStreamProfile {
    AudioDuplexChannels channels{};
    AudioStreamRuntimeCaps runtimeCaps{};
    std::array<DuplexCaptureStreamGeometry, kMaxAudioStreamsPerDirection> captureStreams{};
    std::array<DuplexPlaybackStreamGeometry, kMaxAudioStreamsPerDirection> playbackStreams{};
    Encoding::AudioWireFormat captureWireFormat{Encoding::AudioWireFormat::kAM824};
    Encoding::AudioWireFormat playbackWireFormat{Encoding::AudioWireFormat::kAM824};
    DuplexStartOrderRecipe startOrder{};
    DuplexStopOrderRecipe stopOrder{};
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
        return Build(record, caps, ResolveChannels(record, caps));
    }

    // Device prepare can refresh stream caps after channels have already been
    // assigned and written into the pending restart session. Retain that channel
    // assignment while resolving the refreshed wire geometry.
    [[nodiscard]] static DuplexStreamProfile
    Resolve(const Discovery::DeviceRecord& record, const AudioStreamRuntimeCaps& caps,
            const AudioDuplexChannels& assignedChannels) noexcept {
        return Build(record, caps, assignedChannels);
    }

  private:
    static constexpr uint64_t kAllIsoChannels = ~uint64_t{0};
    static constexpr uint8_t kDefaultCaptureIsoChannel = 1;
    static constexpr uint8_t kDefaultPlaybackIsoChannel = 0;

    // Linux sound/firewire/iso-resources.c:48-76 calculates bandwidth from the
    // maximum CIP payload plus the three-quadlet isoch packet header, scaled to
    // S400 allocation units. Its 512-unit fallback is used when optimized gap
    // count information is unavailable; DeviceRecord currently carries link
    // speed but not the live gap count, so use that conservative reference path.
    [[nodiscard]] static constexpr uint32_t
    PacketBandwidthUnits(uint32_t maxPayloadBytes, FW::FwSpeed speed) noexcept {
        const uint32_t alignedPayloadBytes = (maxPayloadBytes + 3U) & ~3U;
        const uint32_t packetBytesAtSpeed = 12U + alignedPayloadBytes;

        uint32_t packetUnits = packetBytesAtSpeed;
        switch (speed) {
            case FW::FwSpeed::S100:
                packetUnits *= 4U;
                break;
            case FW::FwSpeed::S200:
                packetUnits *= 2U;
                break;
            case FW::FwSpeed::S400:
                break;
            case FW::FwSpeed::S800:
                packetUnits = (packetUnits + 1U) / 2U;
                break;
        }
        return packetUnits + 512U;
    }

    [[nodiscard]] static constexpr uint32_t
    AmdtpBandwidthUnits(uint32_t am824Slots, uint32_t sampleRateHz,
                        FW::FwSpeed speed) noexcept {
        const auto rate = Encoding::AmdtpRateGeometryForSampleRate(
            sampleRateHz != 0 ? sampleRateHz : 48000U);
        const uint32_t blocksPerPacket = rate ? rate->sytIntervalFrames : 8U;
        const uint32_t slots = am824Slots != 0 ? am824Slots : 1U;
        const uint32_t maxPayloadBytes = 8U + blocksPerPacket * slots * 4U;
        return PacketBandwidthUnits(maxPayloadBytes, speed);
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

    [[nodiscard]] static constexpr bool
    HasAlesisCaptureStreamQuirk(const Discovery::DeviceRecord& record) noexcept {
        // FFADO's DICE discovery clamps these Alesis model IDs because they
        // advertise two RX streams although only one exists. Behavioral
        // cross-validation: libffado-2.5.0/src/dice/dice_avdevice.cpp:1682-1695.
        return record.vendorId == DeviceProfiles::Audio::kAlesisVendorId &&
               (record.modelId == 0x000000 || record.modelId == 0x000001);
    }

    [[nodiscard]] static constexpr bool
    IsSPro24Dsp(const Discovery::DeviceRecord& record) noexcept {
        return record.vendorId == DeviceProfiles::Audio::kFocusriteVendorId &&
               record.modelId == DeviceProfiles::Audio::kSPro24DspModelId;
    }

    [[nodiscard]] static constexpr bool
    IsApogeeDuet(const Discovery::DeviceRecord& record) noexcept {
        return record.vendorId == DeviceProfiles::Audio::kApogeeVendorId &&
               record.modelId == DeviceProfiles::Audio::kApogeeDuetModelId;
    }

    [[nodiscard]] static constexpr bool
    IsTerraTecPhase88(const Discovery::DeviceRecord& record) noexcept {
        return record.vendorId == DeviceProfiles::Audio::kTerraTecVendorId &&
               record.modelId == DeviceProfiles::Audio::kPhase88RackFwModelId;
    }

    [[nodiscard]] static AudioDuplexChannels
    ResolveChannels(const Discovery::DeviceRecord& record,
                    const AudioStreamRuntimeCaps& caps) noexcept {
        AudioDuplexChannels channels{
            .deviceToHostIsoChannel = kDefaultCaptureIsoChannel,
            .hostToDeviceIsoChannel = kDefaultPlaybackIsoChannel,
        };

        channels.captureStreamCount = ClampStreamCount(caps.deviceToHostStreamCount);
        channels.playbackStreamCount = ClampStreamCount(caps.hostToDeviceStreamCount);
        if (HasAlesisCaptureStreamQuirk(record)) {
            channels.captureStreamCount = 1;
        }

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
                                                   const AudioDuplexChannels& channels) noexcept {
        DuplexStreamProfile profile{
            .channels = channels,
            .runtimeCaps = caps,
        };

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
            geometry.bandwidthUnits = AmdtpBandwidthUnits(
                geometry.am824Slots, caps.sampleRateHz, record.link.localToNode);
            // CMP (including BridgeCo/BeBoB) does not own a fixed channel;
            // IRM selects one, which is then committed back to its PCR.
            geometry.allowedIsoChannels = (IsApogeeDuet(record) || IsTerraTecPhase88(record))
                                              ? kAllIsoChannels
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
            geometry.bandwidthUnits = AmdtpBandwidthUnits(
                geometry.am824Slots, caps.sampleRateHz, record.link.localToNode);
            geometry.allowedIsoChannels = (IsApogeeDuet(record) || IsTerraTecPhase88(record))
                                              ? kAllIsoChannels
                                              : FixedChannelMask(geometry.isoChannel);
        }

        if (IsSPro24Dsp(record) && caps.hostInputPcmChannels == 8 &&
            caps.deviceToHostAm824Slots == 9) {
            profile.captureWireFormat = Encoding::AudioWireFormat::kRawPcm24In32;
        }
        if (IsSPro24Dsp(record) && caps.hostOutputPcmChannels == 8 &&
            caps.hostToDeviceAm824Slots == 9) {
            profile.playbackWireFormat = Encoding::AudioWireFormat::kRawPcm24In32;
        }
        if (IsApogeeDuet(record)) {
            // Preserve the prior AVCAudioBackend ordering:
            // host IR -> CMP oPCR -> host IT -> CMP iPCR. The runner uses these
            // profile flags to interleave host starts with the neutral device
            // stages. CMP operations retain their adapter-owned 250 ms timeout.
            profile.startOrder.startReceiveBeforeDeviceRx = true;
            profile.startOrder.startTransmitBeforeDeviceTx = true;
            profile.startOrder.postDeviceEnableDelayMs = 0;
            profile.stopOrder
                .disconnectPlaybackThenStopTransmitThenDisconnectCaptureThenStopReceive = true;
        }
        if (IsTerraTecPhase88(record)) {
            // Linux BeBoB reserves both CMP resources, establishes remote
            // iPCR then oPCR, and only then starts the AMDTP domain (RX before
            // TX). Keep that wire-visible ordering explicit rather than
            // inheriting an incidental generic/DICE default. Cross-validated:
            // linux-sound-firewire-stack/firewire/bebob/bebob_stream.c:525-590,
            // 593-674. No reference implementation is copied.
            profile.startOrder.startReceiveBeforeDeviceRx = false;
            profile.startOrder.startTransmitBeforeDeviceTx = false;
            profile.startOrder.requiresPreStreamClockLock = false;
            profile.startOrder.startOrder = {
                DuplexHostDirection::kReceive,
                DuplexHostDirection::kTransmit,
            };
            profile.startOrder.postDeviceEnableDelayMs = 0;
        }
        return profile;
    }
};

} // namespace ASFW::Audio::Backends
