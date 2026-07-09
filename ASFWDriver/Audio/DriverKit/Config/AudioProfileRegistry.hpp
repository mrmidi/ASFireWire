// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioProfileRegistry.hpp
// Global profile registry dispatcher.

#pragma once

#include "IAudioDeviceProfile.hpp"
#include <cstdint>

namespace ASFW::Isoch::Audio {

class AudioProfileRegistry {
public:
    [[nodiscard]] static const IAudioDeviceProfile* FindProfile(uint32_t vendorId,
                                                                uint32_t modelId,
                                                                uint64_t guid) noexcept;
};

} // namespace ASFW::Isoch::Audio
