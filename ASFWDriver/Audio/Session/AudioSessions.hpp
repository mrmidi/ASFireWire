// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioSessions.hpp - The audio session of every device, by GUID.
//
// Owns one SessionScheduler per device and the resources they share. The
// guid-keyed calls are conveniences for callers that hold only a GUID; each
// forwards to that device's scheduler and adds no policy of its own.

#pragma once

#include "SessionScheduler.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ASFW::Audio::Session {

class AudioSessions final {
public:
    AudioSessions(Discovery::DeviceRegistry& registry,
                  AudioRuntimeRegistry& runtime,
                  IIsochDuplexHostTransport& host,
                  Driver::HardwareInterface& hardware,
                  const std::atomic<bool>* teardown,
                  SessionScheduler::BindingSourceProvider bindingSource) noexcept;
    ~AudioSessions() noexcept;

    AudioSessions(const AudioSessions&) = delete;
    AudioSessions& operator=(const AudioSessions&) = delete;

    // Installed by composition before device callbacks begin.
    void SetStartGuard(SessionScheduler::StartGuard guard);

    // The device's session, created on first use. Callers keep the returned
    // pointer for as long as they use it; Erase does not free a session in use.
    [[nodiscard]] std::shared_ptr<SessionScheduler> Ensure(uint64_t guid) noexcept;
    [[nodiscard]] std::shared_ptr<SessionScheduler> Find(uint64_t guid) const noexcept;
    // The device is gone for good: forget its session.
    void Erase(uint64_t guid) noexcept;

    [[nodiscard]] IOReturn Attach(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn Detach(uint64_t guid) noexcept;
    [[nodiscard]] IOReturn ChangeClock(uint64_t guid, const AudioClockConfig& clock,
                                       DuplexRestartReason reason) noexcept;
    [[nodiscard]] IOReturn RequestRestart(uint64_t guid, DuplexRestartReason reason,
                                          uint64_t observedRun = 0) noexcept;
    void Retire(uint64_t guid) noexcept;
    void Present(uint64_t guid) noexcept;

    [[nodiscard]] bool IsStreaming(uint64_t guid) const noexcept;
    [[nodiscard]] uint64_t RunningRun(uint64_t guid) const noexcept;
    [[nodiscard]] bool IsReconciling(uint64_t guid) const noexcept;
    // Service teardown, or the device retired: device work must not start.
    [[nodiscard]] bool IsCancelled(uint64_t guid) const noexcept;
    [[nodiscard]] std::optional<SessionSnapshot> Snapshot(uint64_t guid) const noexcept;
    [[nodiscard]] std::vector<uint64_t> StreamingGuids() const noexcept;
    [[nodiscard]] uint64_t TeardownAbortCount() const noexcept {
        return teardownAborts_.load(std::memory_order_acquire);
    }

private:
    [[nodiscard]] bool TeardownRequested() const noexcept;

    Discovery::DeviceRegistry& registry_;
    AudioRuntimeRegistry& runtime_;
    IIsochDuplexHostTransport& host_;
    Driver::HardwareInterface& hardware_;
    const std::atomic<bool>* teardown_;
    SessionScheduler::BindingSourceProvider bindingSource_;
    SessionScheduler::StartGuard startGuard_;
    std::atomic<uint64_t> teardownAborts_{0};

    IOLock* lock_{nullptr};
    std::unordered_map<uint64_t, std::shared_ptr<SessionScheduler>> sessions_;
};

} // namespace ASFW::Audio::Session
