// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// StopRoutine.hpp - Stop everything a start may have left running.
//
// documentation/AUDIO_SESSION_REDESIGN.md §4.2. The stop does not need to know
// how far a start got: it stops every host context and asks the family to stop
// the device, whatever state either is in. The order follows the device's stop
// recipe (DuplexStreamProfile), resolved from the catalog.

#pragma once

#include "../Protocols/Duplex/FamilyDriver.hpp"

#include "../Protocols/Backends/IsochDuplexHostTransport.hpp"
#include "../../Discovery/DeviceRegistry.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::Session {

class StopRoutine final {
public:
    StopRoutine(Discovery::DeviceRegistry& registry,
                IIsochDuplexHostTransport& host,
                const std::atomic<bool>* teardown,
                std::atomic<uint64_t>& teardownAborts) noexcept;

    // Stop host and device. `caps` and `channels` are the ones the streams were
    // started with; they select the stop recipe.
    [[nodiscard]] IOReturn Run(uint64_t guid,
                               const Discovery::DeviceRecord& record,
                               FamilyDriver& family,
                               const AudioStreamRuntimeCaps& caps,
                               const AudioDuplexChannels& channels) noexcept;

    // After a failed start: drop the device connections, then stop.
    [[nodiscard]] IOReturn Rollback(uint64_t guid,
                                    const Discovery::DeviceRecord& record,
                                    const Discovery::DeviceRouteToken& route,
                                    FamilyDriver& family,
                                    const AudioStreamRuntimeCaps& caps,
                                    const AudioDuplexChannels& channels) noexcept;

private:
    [[nodiscard]] bool TeardownRequested() const noexcept;
    void RecordTeardownAbort(const char* stage, uint64_t guid) noexcept;

    Discovery::DeviceRegistry& registry_;
    IIsochDuplexHostTransport& host_;
    const std::atomic<bool>* teardown_;
    std::atomic<uint64_t>& teardownAborts_;
};

} // namespace ASFW::Audio::Session
