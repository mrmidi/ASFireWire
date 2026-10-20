// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IAudioEndpointObserver.hpp - Who else cares that an audio endpoint exists.
//
// The audio device host publishes and retires endpoints for every family. A
// feature that rides an endpoint without being part of its audio (MIDI) learns
// of it here instead of the host growing a branch per feature. The host calls
// these after its own decision and never waits for the outcome; an observer
// failing must not change what happened to the audio endpoint.

#pragma once

#include "../Model/ASFWAudioDevice.hpp"
#include "../Protocols/DeviceProtocolChoice.hpp"

#include <cstdint>

namespace ASFW::Audio::Host {

class IAudioEndpointObserver {
public:
    virtual ~IAudioEndpointObserver() = default;

    /// The endpoint for `guid` is published (first publication) or confirmed
    /// unchanged (a refresh). `description` is the configuration the audio nub
    /// carries. Idempotent: called again for a live endpoint.
    virtual void OnEndpointPublished(uint64_t guid, AudioBackendKind kind,
                                     const Model::ASFWAudioDevice& description) noexcept = 0;

    /// The endpoint is going away. Called before its runtime is removed.
    virtual void OnEndpointRetired(uint64_t guid) noexcept = 0;
};

} // namespace ASFW::Audio::Host
