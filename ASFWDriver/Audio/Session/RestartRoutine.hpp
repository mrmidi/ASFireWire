// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RestartRoutine.hpp - Bring the streams up, in wire order, top to bottom.
//
// documentation/AUDIO_SESSION_REDESIGN.md §4.2. One linear sequence for every
// family; the family answers each step, and the catalog's start recipe
// (DuplexStreamProfile) decides where host starts interleave with device steps.
// Every failure rolls back through StopRoutine, except a service teardown,
// which returns at once so no MMIO follows the hardware detach.

#pragma once

#include "FamilyDriver.hpp"
#include "StopRoutine.hpp"

#include "../Protocols/Backends/IsochDuplexHostTransport.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Hardware/HardwareInterface.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>

namespace ASFW::IRM {
class IRMClient;
}

namespace ASFW::Audio::Runtime {
class IDirectAudioBindingSource;
}

namespace ASFW::Audio::Session {

// What runs after a successful restart.
struct RunningSession {
    Discovery::DeviceRouteToken route{};
    FW::Generation generation{0};
    AudioDuplexChannels channels{};
    AudioClockConfig appliedClock{};
    AudioStreamRuntimeCaps runtimeCaps{};
};

// Why a restart did not reach Running. The rollback has already run, unless
// `stopped` is false (a teardown abort, or a refusal before anything started).
struct RestartFailure {
    IOReturn status{kIOReturnError};
    const char* step{""};
    bool stopped{true};
    AudioDuplexChannels channels{};
    AudioStreamRuntimeCaps runtimeCaps{};
};

class RestartRoutine final {
public:
    struct Dependencies {
        Discovery::DeviceRegistry& registry;
        IIsochDuplexHostTransport& host;
        Driver::HardwareInterface& hardware;
        StopRoutine& stop;
        const std::atomic<bool>* teardown;
        std::atomic<uint64_t>& teardownAborts;
    };

    struct Request {
        uint64_t guid{0};
        // The device as the session found it; the routine refreshes it after
        // reading the stream geometry.
        Discovery::DeviceRecord record{};
        FamilyDriver* family{nullptr};
        ::ASFW::IRM::IRMClient* irm{nullptr};
        Runtime::IDirectAudioBindingSource* binding{nullptr};
        AudioClockConfig clock{};
        DuplexRestartReason reason{DuplexRestartReason::kInitialStart};
        // True when the session no longer wants this start: a stop was
        // requested or the device was retired. Checked between steps.
        std::function<bool()> superseded;
    };

    static constexpr uint32_t kClockLockTimeoutMs = 1000;
    static constexpr uint32_t kClockLockPollMs = 10;
    static constexpr uint32_t kClockStableReads = 3;

    explicit RestartRoutine(Dependencies dependencies) noexcept : deps_(dependencies) {}

    [[nodiscard]] std::expected<RunningSession, RestartFailure> Run(const Request& request) noexcept;

private:
    [[nodiscard]] bool TeardownRequested() const noexcept;
    void RecordTeardownAbort(const char* stage, uint64_t guid) noexcept;
    [[nodiscard]] IOReturn AwaitStableClock(FamilyDriver& family, FW::Generation generation,
                                            const AudioClockConfig& clock, uint64_t guid) noexcept;

    Dependencies deps_;
};

} // namespace ASFW::Audio::Session
