// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ASFWAudioDevice.hpp"

namespace ASFW::Audio::Model {

// The device supports this rate, but its layout is still a placeholder.
[[nodiscard]] inline bool LayoutIsProvisional(const ASFWAudioDevice& config, uint32_t rateHz) noexcept {
    const auto formation = std::ranges::find(config.rateFormationCandidates, rateHz,
        &Runtime::RateFormation::sampleRateHz);
    return formation != config.rateFormationCandidates.end() && formation->layoutProvisional;
}

// Resolve from retained descriptor formations, never by scaling channel counts.
// This is a value copy; callers install it only with both directions quiesced.
[[nodiscard]] inline std::expected<ASFWAudioDevice, Runtime::ConfigurationError>
WithRateFormation(const ASFWAudioDevice& prior, uint32_t rateHz) {
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
                stream.midiPortCount ? stream.midiPortCount : stream.midiSlots, offset, stream.pcmSlots, stream.packedPcmChunks});
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

// Add one measured formation without changing the committed live format.
// A known formation must agree: stale registers must not overwrite an EAP
// catalog. A provisional one is a placeholder and takes the measured layout.
// The caller supplies a stable, route-checked hardware observation.
[[nodiscard]] inline std::expected<ASFWAudioDevice, Runtime::ConfigurationError>
WithObservedRateCatalog(const ASFWAudioDevice& prior, const Runtime::RateFormation& observed) {
    if (observed.layoutProvisional) return std::unexpected(Runtime::ConfigurationError::InvalidFormation);
    auto next = prior;
    const auto existing = std::ranges::find(next.rateFormationCandidates,
        observed.sampleRateHz, &Runtime::RateFormation::sampleRateHz);
    if (existing == next.rateFormationCandidates.end()) {
        next.rateFormationCandidates.push_back(observed);
        std::ranges::sort(next.rateFormationCandidates, {}, &Runtime::RateFormation::sampleRateHz);
    } else if (existing->layoutProvisional) {
        *existing = observed;
    }
    const auto selected = WithRateFormation(next, observed.sampleRateHz);
    if (!selected) return std::unexpected(selected.error());
    const auto& retained = *std::ranges::find(next.rateFormationCandidates,
        observed.sampleRateHz, &Runtime::RateFormation::sampleRateHz);
    if (retained.mode != observed.mode || retained.playback != observed.playback ||
        retained.capture != observed.capture)
        return std::unexpected(Runtime::ConfigurationError::InvalidFormation);
    if (std::ranges::find(next.sampleRates, observed.sampleRateHz) == next.sampleRates.end()) {
        next.sampleRates.push_back(observed.sampleRateHz);
        std::ranges::sort(next.sampleRates);
    }
    return next;
}

} // namespace ASFW::Audio::Model
