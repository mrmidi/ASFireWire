// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IAVCAudioConfigListener.hpp
// Minimal sink interface for AV/C discovery to publish audio configuration
// without owning AudioNub lifetime or isoch transport.

#pragma once

#include <cstdint>

namespace ASFW::Audio::Model {
struct ASFWAudioDevice;
}

namespace ASFW::Audio {

class IAVCAudioConfigListener {
public:
    virtual ~IAVCAudioConfigListener() = default;

    virtual void OnAVCAudioConfigurationReady(uint64_t guid,
                                              const Model::ASFWAudioDevice& config) noexcept = 0;
};

} // namespace ASFW::Audio

