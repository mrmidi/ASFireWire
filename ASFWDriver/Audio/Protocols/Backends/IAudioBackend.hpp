// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// IAudioBackend.hpp
// Audio backend interface used by AudioCoordinator to decouple control-plane policy.

#pragma once

#include <DriverKit/IOReturn.h>
#include <cstdint>

namespace ASFW::Audio {

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;

    [[nodiscard]] virtual const char* Name() const noexcept = 0;

    [[nodiscard]] virtual IOReturn StartStreaming(uint64_t guid) noexcept = 0;
    [[nodiscard]] virtual IOReturn StopStreaming(uint64_t guid) noexcept = 0;
};

} // namespace ASFW::Audio

