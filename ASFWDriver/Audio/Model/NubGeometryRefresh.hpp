// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// NubGeometryRefresh.hpp — may a live endpoint's properties be re-published?
//
// The nub is a serialized contract between two IOService objects, and the audio
// side reads it ONCE, during graph setup: ParseAudioDriverConfigFromProperties
// fills ParsedAudioDriverConfig, CopyParsedConfigToDeviceState copies it into
// ivars.device, and ASFWAudioDevice::StartIO frames every stream from there.
//
// So SetProperties() on a live nub does not reach StartIO. Re-publishing
// CHANGED geometry would leave the nub describing one device and the audio side
// framing another -- strictly worse than leaving it stale, because the two
// descriptions would then disagree with each other as well as with the wire.
//
// HAL channel topology and supported rates are cached too; changing those
// requires endpoint recreation, even when the wire streams are unchanged.
// Current sample rate is managed by the existing clock-change contract.
//
// Changed geometry therefore has exactly one correct outcome today: refuse, say
// so, and leave the endpoint on the description it was built with until it is
// recreated. Live reconfiguration must coordinate the audio graph, buffers,
// and HAL channel layout; updating properties alone is insufficient. See
// documentation/DICE_TCAT_ARCHITECTURE.md sec 3.4 and 4.2 step D.

#pragma once

#include "ASFWAudioDevice.hpp"

#include <cstdint>
#include <vector>

namespace ASFW::Audio::Model {

enum class GeometryRefreshDecision : uint8_t {
    /// The existing endpoint configuration can be retained.
    kMayRefresh,
    /// Geometry differs from what the live endpoint was built with. Refuse.
    kGeometryChanged,
};

[[nodiscard]] inline bool
SameWireStreams(const std::vector<ASFWAudioWireStream>& published,
                const std::vector<ASFWAudioWireStream>& incoming) noexcept {
    if (published.size() != incoming.size()) {
        return false;
    }
    for (size_t i = 0; i < published.size(); ++i) {
        if (!(published[i] == incoming[i])) {
            return false;
        }
    }
    return true;
}

/// Both directions are compared. Capture framing is device-sourced through a
/// different path than playback, but the HAL's input channel layout comes from
/// this same snapshot, so a capture change is no more re-readable than a
/// playback one.
[[nodiscard]] inline GeometryRefreshDecision
ClassifyGeometryRefresh(const ASFWAudioDevice& published,
                        const ASFWAudioDevice& incoming) noexcept {
    const bool same = SameWireStreams(published.playbackStreams, incoming.playbackStreams) &&
                      SameWireStreams(published.captureStreams, incoming.captureStreams) &&
                      published.resolvedGeometryRequired == incoming.resolvedGeometryRequired &&
                      published.inputChannelCount == incoming.inputChannelCount &&
                      published.outputChannelCount == incoming.outputChannelCount &&
                      published.channelCount == incoming.channelCount &&
                      published.sampleRates == incoming.sampleRates &&
                      published.deviceSampleRates == incoming.deviceSampleRates &&
                      published.streamMode == incoming.streamMode;
    return same ? GeometryRefreshDecision::kMayRefresh
                : GeometryRefreshDecision::kGeometryChanged;
}

// A mismatch stays latched even if a later observation matches again. Only
// destroying/recreating the published endpoint establishes a new contract.
class NubGeometryRefreshState {
public:
    explicit NubGeometryRefreshState(const ASFWAudioDevice& published)
        : published_(published) {}
    [[nodiscard]] bool Accept(const ASFWAudioDevice& incoming) noexcept {
        blocked_ = blocked_ ||
            ClassifyGeometryRefresh(published_, incoming) == GeometryRefreshDecision::kGeometryChanged;
        return !blocked_;
    }
    [[nodiscard]] bool IsBlocked() const noexcept { return blocked_; }
private:
    ASFWAudioDevice published_;
    bool blocked_{false};
};

} // namespace ASFW::Audio::Model
