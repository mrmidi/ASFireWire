// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "DuplexPolicies.hpp"
#include "../Devices/ResolvedAudioEndpointProfile.hpp"
#include "../Wire/AMDTP/AmdtpRateGeometry.hpp"

#include <array>
#include <cstdint>

namespace ASFW::Audio::Duplex {

struct CaptureStreamGeometry final {
    uint8_t isoChannel{AudioStreamWireInfo::kInvalidIsoChannel};
    uint32_t pcmChannelOffset{0};
    uint32_t pcmChannels{0};
    uint32_t am824Slots{0};
    uint32_t bandwidthUnits{0};
    uint64_t allowedIsoChannels{0};
};

struct PlaybackStreamGeometry final {
    uint8_t isoChannel{AudioStreamWireInfo::kInvalidIsoChannel};
    uint32_t pcmChannels{0};
    uint32_t am824Slots{0};
    uint32_t bandwidthUnits{0};
    uint64_t allowedIsoChannels{0};
};

struct StreamPlan final {
    AudioDuplexChannels channels{};
    AudioStreamRuntimeCaps runtimeCaps{};
    std::array<CaptureStreamGeometry, kMaxAudioStreamsPerDirection> captureStreams{};
    std::array<PlaybackStreamGeometry, kMaxAudioStreamsPerDirection> playbackStreams{};
    Encoding::AudioWireFormat captureWireFormat{Encoding::AudioWireFormat::kAM824};
    Encoding::AudioWireFormat playbackWireFormat{Encoding::AudioWireFormat::kAM824};
    StartPolicy startOrder{};
    StopPolicy stopOrder{};
};

// Pure planner: its inputs are a fully resolved endpoint profile, current link
// speed, and optionally a previously allocated channel set. It contains no
// GUID/vendor/model/descriptor matching.
class StreamPlanner final {
public:
    [[nodiscard]] static StreamPlan Resolve(
        const Devices::ResolvedAudioEndpointProfile& profile,
        FW::FwSpeed linkSpeed) noexcept {
        return Build(profile, linkSpeed, ResolveChannels(profile));
    }

    [[nodiscard]] static StreamPlan Resolve(
        const Devices::ResolvedAudioEndpointProfile& profile,
        FW::FwSpeed linkSpeed,
        const AudioDuplexChannels& assignedChannels) noexcept {
        return Build(profile, linkSpeed, assignedChannels);
    }

private:
    static constexpr uint64_t kAllIsoChannels = ~uint64_t{0};
    static constexpr uint8_t kDefaultCaptureIsoChannel = 1;
    static constexpr uint8_t kDefaultPlaybackIsoChannel = 0;

    [[nodiscard]] static constexpr bool IsValidIsoChannel(uint8_t channel) noexcept {
        return channel <= 0x3F;
    }

    [[nodiscard]] static constexpr uint32_t ClampStreamCount(uint32_t count) noexcept {
        if (count == 0) {
            return 1;
        }
        return count > kMaxAudioStreamsPerDirection ? kMaxAudioStreamsPerDirection : count;
    }

    [[nodiscard]] static constexpr uint64_t FixedChannelMask(uint8_t channel) noexcept {
        return IsValidIsoChannel(channel) ? (uint64_t{1} << channel) : 0;
    }

    // Cross-validated with Linux sound/firewire/iso-resources.c:48-76. The
    // conservative 512-unit fallback is used because live gap count is not an
    // input to this planner yet.
    [[nodiscard]] static constexpr uint32_t PacketBandwidthUnits(
        uint32_t maxPayloadBytes, FW::FwSpeed speed) noexcept {
        const uint32_t packetBytesAtSpeed = 12U + ((maxPayloadBytes + 3U) & ~3U);
        uint32_t packetUnits = packetBytesAtSpeed;
        switch (speed) {
            case FW::FwSpeed::S100: packetUnits *= 4U; break;
            case FW::FwSpeed::S200: packetUnits *= 2U; break;
            case FW::FwSpeed::S400: break;
            case FW::FwSpeed::S800: packetUnits = (packetUnits + 1U) / 2U; break;
        }
        return packetUnits + 512U;
    }

    [[nodiscard]] static constexpr uint32_t AmdtpBandwidthUnits(
        uint32_t slots, uint32_t sampleRateHz, FW::FwSpeed speed) noexcept {
        const auto rate = Encoding::AmdtpRateGeometryForSampleRate(
            sampleRateHz != 0 ? sampleRateHz : 48000U);
        const uint32_t blocks = rate ? rate->sytIntervalFrames : 8U;
        return PacketBandwidthUnits(8U + blocks * (slots != 0 ? slots : 1U) * 4U,
                                    speed);
    }

    [[nodiscard]] static AudioDuplexChannels ResolveChannels(
        const Devices::ResolvedAudioEndpointProfile& profile) noexcept {
        const auto& caps = profile.runtimeCaps;
        AudioDuplexChannels channels{
            .deviceToHostIsoChannel = kDefaultCaptureIsoChannel,
            .hostToDeviceIsoChannel = kDefaultPlaybackIsoChannel,
        };
        channels.captureStreamCount = ClampStreamCount(caps.deviceToHostStreamCount);
        channels.playbackStreamCount = ClampStreamCount(caps.hostToDeviceStreamCount);

        uint64_t usedChannels = 0;
        const auto markUsed = [&usedChannels](uint8_t channel) noexcept {
            if (channel <= 0x3F) usedChannels |= uint64_t{1} << channel;
        };
        const auto nextFree = [&usedChannels]() noexcept -> uint8_t {
            for (uint8_t channel = 0; channel <= 0x3F; ++channel) {
                const uint64_t bit = uint64_t{1} << channel;
                if ((usedChannels & bit) == 0) {
                    usedChannels |= bit;
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

    [[nodiscard]] static StreamPlan Build(
        const Devices::ResolvedAudioEndpointProfile& profile,
        FW::FwSpeed linkSpeed,
        const AudioDuplexChannels& channels) noexcept {
        const auto& caps = profile.runtimeCaps;
        StreamPlan result{
            .channels = channels,
            .runtimeCaps = caps,
            .captureWireFormat = profile.captureWireFormat,
            .playbackWireFormat = profile.playbackWireFormat,
            .startOrder = profile.startPolicy,
            .stopOrder = profile.stopPolicy,
        };

        const bool multiCapture = channels.captureStreamCount > 1;
        uint32_t captureOffset = 0;
        for (uint32_t i = 0; i < channels.captureStreamCount; ++i) {
            const auto& stream = caps.deviceToHostStreams[i];
            auto& geometry = result.captureStreams[i];
            geometry.isoChannel = channels.CaptureChannel(i);
            geometry.pcmChannelOffset = captureOffset;
            geometry.pcmChannels = multiCapture ? stream.pcmChannels : 0;
            geometry.am824Slots = multiCapture ? stream.am824Slots
                                               : caps.deviceToHostAm824Slots;
            geometry.bandwidthUnits = AmdtpBandwidthUnits(
                geometry.am824Slots, caps.sampleRateHz, linkSpeed);
            geometry.allowedIsoChannels =
                profile.captureIsoChannelPolicy == IsoChannelPolicy::IRMSelectable
                    ? kAllIsoChannels
                    : FixedChannelMask(geometry.isoChannel);
            captureOffset += geometry.pcmChannels;
        }

        for (uint32_t i = 0; i < channels.playbackStreamCount; ++i) {
            const auto& stream = caps.hostToDeviceStreams[i];
            auto& geometry = result.playbackStreams[i];
            geometry.isoChannel = channels.PlaybackChannel(i);
            geometry.pcmChannels = stream.pcmChannels != 0
                                       ? stream.pcmChannels
                                       : (i == 0 ? caps.hostOutputPcmChannels : 0U);
            geometry.am824Slots = stream.am824Slots != 0
                                      ? stream.am824Slots
                                      : (i == 0 ? caps.hostToDeviceAm824Slots : 0U);
            geometry.bandwidthUnits = AmdtpBandwidthUnits(
                geometry.am824Slots, caps.sampleRateHz, linkSpeed);
            geometry.allowedIsoChannels =
                profile.playbackIsoChannelPolicy == IsoChannelPolicy::IRMSelectable
                    ? kAllIsoChannels
                    : FixedChannelMask(geometry.isoChannel);
        }
        return result;
    }
};

} // namespace ASFW::Audio::Duplex
