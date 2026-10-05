// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ResolvedTimingGeometry.hpp"
#include "../Wire/AMDTP/PcmSlotMap.hpp"

#include <span>
#include <vector>

namespace ASFW::Audio::Runtime {

// A formation describes one rate, not the shape observed at discovery time.
// Protocol adapters supply these facts; generic code never scales ADAT widths.
struct RateWireStream final {
    uint32_t pcmChannels{0};
    uint32_t dataBlockSize{0};
    uint32_t midiSlots{0};
    Wire::PcmSlotMap pcmSlots{};
    friend bool operator==(const RateWireStream&, const RateWireStream&) = default;
};

struct RateFormation final {
    uint32_t sampleRateHz{0};
    Encoding::StreamMode mode{Encoding::StreamMode::kBlocking};
    std::vector<RateWireStream> playback;
    std::vector<RateWireStream> capture;
    bool protocolSupported{false};
    bool hardwareValidated{false};
    friend bool operator==(const RateFormation&, const RateFormation&) = default;
};

struct ConfigurationAllocation final {
    uint32_t frameCapacity{0};
    uint32_t playbackChannelCapacity{0};
    uint32_t captureChannelCapacity{0};
    uint32_t maxPacketBytes{0};
    friend bool operator==(const ConfigurationAllocation&, const ConfigurationAllocation&) = default;
};

enum class ConfigurationError : uint8_t {
    RateNotOffered, ProtocolUnsupported, HardwareUnvalidated,
    InvalidFormation, InvalidTiming, ExceedsAllocation,
};

struct ResolvedAudioConfiguration final {
    uint64_t revision{0};
    RateFormation formation;
    ResolvedTimingGeometry timing;
    ConfigurationAllocation allocation;
    uint32_t playbackChannels{0};
    uint32_t captureChannels{0};
    uint32_t maxPacketFrames{0};
    uint64_t playbackAllocationBytes{0};
    uint64_t captureAllocationBytes{0};
};

// Returned values become immutable snapshots at the coordinator boundary.
// Validation is side-effect free and shares the existing timing authority.
[[nodiscard]] inline std::expected<ResolvedAudioConfiguration, ConfigurationError>
ResolveAudioConfiguration(uint32_t rate, std::span<const RateFormation> formations,
                          const DeviceTimingPolicy& policy,
                          ConfigurationAllocation allocation,
                          uint64_t revision) {
    const RateFormation* selected = nullptr;
    for (const auto& formation : formations) {
        if (formation.sampleRateHz != rate) continue;
        if (selected) return std::unexpected(ConfigurationError::InvalidFormation);
        selected = &formation;
    }
    if (!selected) return std::unexpected(ConfigurationError::RateNotOffered);
    if (!selected->protocolSupported) return std::unexpected(ConfigurationError::ProtocolUnsupported);
    if (!selected->hardwareValidated) return std::unexpected(ConfigurationError::HardwareUnvalidated);
    if ((selected->mode != Encoding::StreamMode::kBlocking &&
         selected->mode != Encoding::StreamMode::kNonBlocking) ||
        allocation.playbackChannelCapacity > Encoding::kMaxPcmChannels ||
        allocation.captureChannelCapacity > Encoding::kMaxPcmChannels)
        return std::unexpected(ConfigurationError::InvalidFormation);
    const auto timing = ResolveTimingGeometry(rate, selected->mode, policy, allocation.frameCapacity);
    if (!timing) return std::unexpected(ConfigurationError::InvalidTiming);
    const auto geometry = Encoding::AmdtpRateGeometryForSampleRate(rate);
    const uint32_t packetFrames = selected->mode == Encoding::StreamMode::kBlocking
        ? geometry->sytIntervalFrames : geometry->nominalFramesPerCycle;
    if (selected->playback.empty() && selected->capture.empty())
        return std::unexpected(ConfigurationError::InvalidFormation);
    const auto validateDirection = [&](const auto& streams, uint32_t capacity)
        -> std::expected<uint32_t, ConfigurationError> {
        uint32_t channels = 0;
        for (const auto& stream : streams) {
            if (!stream.pcmChannels || stream.pcmChannels > Encoding::kMaxPcmChannels ||
                stream.dataBlockSize > Encoding::kMaxAmdtpDbs ||
                stream.midiSlots > stream.dataBlockSize ||
                stream.pcmChannels > stream.dataBlockSize - stream.midiSlots ||
                !stream.pcmSlots.FitsWithin(stream.pcmChannels, stream.dataBlockSize))
                return std::unexpected(ConfigurationError::InvalidFormation);
            uint32_t used = 0;
            for (uint32_t channel = 0; channel < stream.pcmChannels; ++channel) {
                const uint32_t bit = uint32_t{1} << stream.pcmSlots.SlotFor(channel);
                if (used & bit) return std::unexpected(ConfigurationError::InvalidFormation);
                used |= bit;
            }
            if (uint64_t{8} + uint64_t(packetFrames) * stream.dataBlockSize * 4 > allocation.maxPacketBytes)
                return std::unexpected(ConfigurationError::ExceedsAllocation);
            if (stream.pcmChannels > capacity || channels > capacity - stream.pcmChannels)
                return std::unexpected(ConfigurationError::ExceedsAllocation);
            channels += stream.pcmChannels;
        }
        return channels;
    };
    const auto playback = validateDirection(selected->playback, allocation.playbackChannelCapacity);
    if (!playback) return std::unexpected(playback.error());
    const auto capture = validateDirection(selected->capture, allocation.captureChannelCapacity);
    if (!capture) return std::unexpected(capture.error());
    return ResolvedAudioConfiguration{
        .revision = revision, .formation = *selected, .timing = *timing,
        .allocation = allocation, .playbackChannels = *playback, .captureChannels = *capture,
        .maxPacketFrames = packetFrames,
        .playbackAllocationBytes = uint64_t(allocation.frameCapacity) * allocation.playbackChannelCapacity * 4,
        .captureAllocationBytes = uint64_t(allocation.frameCapacity) * allocation.captureChannelCapacity * 4,
    };
}

} // namespace ASFW::Audio::Runtime
