// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ASFWAudioDevice.hpp"

namespace ASFW::Audio::Model {

// Resolve from retained descriptor formations, never by scaling channel counts.
// This is a value copy; callers install it only with both directions quiesced.
[[nodiscard]] inline std::expected<ASFWAudioDevice, Runtime::ConfigurationError>
WithAvcRateFormation(const ASFWAudioDevice& prior, uint32_t rateHz) {
    const auto allocation = Runtime::MaximumFormationAllocation(prior.rateFormationCandidates,
        {ASFW::IsochTransport::kAllocatedFrameRingFrames, prior.outputChannelCount, prior.inputChannelCount, 0});
    if (!allocation) return std::unexpected(allocation.error());
    const auto resolved = Runtime::ResolveAudioConfiguration(rateHz, prior.rateFormationCandidates,
        {64, 64, 128, 128}, *allocation, 0, Runtime::ConfigurationValidationPolicy::HardwareBatch);
    if (!resolved) return std::unexpected(resolved.error());
    if (resolved->formation.playback.empty() || resolved->formation.capture.empty())
        return std::unexpected(Runtime::ConfigurationError::InvalidFormation);
    auto next = prior;
    next.currentSampleRate = rateHz;
    next.outputChannelCount = resolved->playbackChannels;
    next.inputChannelCount = resolved->captureChannels;
    next.channelCount = std::max(next.outputChannelCount, next.inputChannelCount);
    next.streamMode = resolved->formation.mode == Encoding::StreamMode::kBlocking
        ? StreamMode::kBlocking : StreamMode::kNonBlocking;
    const auto convert = [](const auto& direction) {
        std::vector<ASFWAudioWireStream> result;
        uint32_t offset = 0;
        for (const auto& stream : direction) {
            result.push_back({stream.pcmChannels, stream.dataBlockSize,
                stream.midiSlots, offset, stream.pcmSlots});
            offset += stream.pcmChannels;
        }
        return result;
    };
    next.playbackStreams = convert(resolved->formation.playback);
    next.captureStreams = convert(resolved->formation.capture);
    // Old per-channel labels may describe unmultiplexed ADAT slots. Keep names
    // only for an unchanged formation; the driver synthesizes names otherwise.
    if (next.playbackStreams != prior.playbackStreams) next.outputChannelNames.clear();
    if (next.captureStreams != prior.captureStreams) next.inputChannelNames.clear();
    return next;
}

} // namespace ASFW::Audio::Model
