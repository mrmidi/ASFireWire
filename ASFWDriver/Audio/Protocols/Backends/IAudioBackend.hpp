// SPDX-License-Identifier: Apache-2.0
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

    virtual void OnDeviceRecordUpdated(uint64_t guid) noexcept { (void)guid; }
    virtual void OnDeviceResumed(uint64_t guid) noexcept { (void)guid; }
    virtual void CancelRemoteDeviceWork(uint64_t guid) noexcept = 0;
    virtual void HandleHostTimingLoss(uint64_t guid) noexcept { (void)guid; }
    virtual void HandleCycleInconsistent(uint64_t guid) noexcept { (void)guid; }
    // A restart request rebuilt the device's streams (at once, or after the
    // family's quiet period). Families that republish geometry do it here.
    virtual void OnStreamsRestarted(uint64_t guid) noexcept { (void)guid; }

    virtual void BeginTeardown() noexcept = 0;
};

} // namespace ASFW::Audio

