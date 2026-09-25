// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioSessions.hpp"

#include <utility>
#include <vector>

namespace ASFW::Audio::Session {

AudioSessions::AudioSessions(Discovery::DeviceRegistry& registry,
                             AudioRuntimeRegistry& runtime,
                             IIsochDuplexHostTransport& host,
                             Driver::HardwareInterface& hardware,
                             const std::atomic<bool>* teardown,
                             SessionScheduler::BindingSourceProvider bindingSource) noexcept
    : registry_(registry),
      runtime_(runtime),
      host_(host),
      hardware_(hardware),
      teardown_(teardown),
      bindingSource_(std::move(bindingSource)) {
    lock_ = IOLockAlloc();
    IODispatchQueue* queue = nullptr;
    if (IODispatchQueue::Create("com.asfw.audio.sessions", 0, 0, &queue) == kIOReturnSuccess &&
        queue != nullptr) {
        queue_ = OSSharedPtr(queue, OSNoRetain);
    }
}

AudioSessions::~AudioSessions() noexcept {
    BeginTeardown();
    if (lock_ != nullptr) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void AudioSessions::SetStartGuard(SessionScheduler::StartGuard guard) {
    startGuard_ = std::move(guard);
}

void AudioSessions::SetRestartObserver(SessionScheduler::RestartObserver observer) {
    restartObserver_ = std::move(observer);
}

void AudioSessions::BeginTeardown() noexcept {
    std::vector<std::shared_ptr<SessionScheduler>> sessions;
    if (lock_ != nullptr) {
        IOLockLock(lock_);
        for (const auto& [guid, session] : sessions_) {
            sessions.push_back(session);
        }
        IOLockUnlock(lock_);
    }
    for (const auto& session : sessions) {
        session->CancelPendingRestart();
    }
    if (queue_) {
#ifdef ASFW_HOST_TEST
        queue_->DispatchSync([] {});
#else
        queue_->DispatchSync(^{});
#endif
    }
}

bool AudioSessions::TeardownRequested() const noexcept {
    return teardown_ != nullptr && teardown_->load(std::memory_order_acquire);
}

std::shared_ptr<SessionScheduler> AudioSessions::Ensure(uint64_t guid) noexcept {
    if (guid == 0 || lock_ == nullptr) {
        return nullptr;
    }
    IOLockLock(lock_);
    auto& slot = sessions_[guid];
    if (!slot) {
        slot = std::make_shared<SessionScheduler>(
            guid, SessionScheduler::Dependencies{
                      .registry = registry_,
                      .runtime = runtime_,
                      .host = host_,
                      .hardware = hardware_,
                      .teardown = teardown_,
                      .teardownAborts = teardownAborts_,
                      .bindingSource = bindingSource_,
                      .startGuard = &startGuard_,
                      .timer = &timer_,
                      .queue = queue_.get(),
                      .restartObserver = &restartObserver_,
                  });
    }
    auto session = slot;
    IOLockUnlock(lock_);
    return session;
}

std::shared_ptr<SessionScheduler> AudioSessions::Find(uint64_t guid) const noexcept {
    if (guid == 0 || lock_ == nullptr) {
        return nullptr;
    }
    IOLockLock(lock_);
    const auto it = sessions_.find(guid);
    auto session = it != sessions_.end() ? it->second : nullptr;
    IOLockUnlock(lock_);
    return session;
}

void AudioSessions::Erase(uint64_t guid) noexcept {
    if (lock_ == nullptr) {
        return;
    }
    std::shared_ptr<SessionScheduler> session;
    IOLockLock(lock_);
    if (const auto it = sessions_.find(guid); it != sessions_.end()) {
        session = std::move(it->second);
        sessions_.erase(it);
    }
    IOLockUnlock(lock_);
    if (session) {
        session->CancelPendingRestart();
    }
}

IOReturn AudioSessions::Attach(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    const auto session = Ensure(guid);
    return session ? session->Attach() : kIOReturnNoResources;
}

IOReturn AudioSessions::Detach(uint64_t guid) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    const auto session = Ensure(guid);
    return session ? session->Detach() : kIOReturnNoResources;
}

IOReturn AudioSessions::ChangeClock(uint64_t guid, const AudioClockConfig& clock,
                                    DuplexRestartReason reason) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    const auto session = Ensure(guid);
    return session ? session->ChangeClock(clock, reason) : kIOReturnNoResources;
}

IOReturn AudioSessions::RequestRestart(uint64_t guid, DuplexRestartReason reason,
                                       uint64_t observedRun) noexcept {
    if (guid == 0) {
        return kIOReturnBadArgument;
    }
    const auto session = Find(guid);
    return session ? session->RequestRestart(reason, observedRun) : kIOReturnUnsupported;
}

void AudioSessions::Retire(uint64_t guid) noexcept {
    if (const auto session = Ensure(guid)) {
        session->Retire();
    }
}

void AudioSessions::Present(uint64_t guid) noexcept {
    if (const auto session = Find(guid)) {
        session->Present();
    }
}

bool AudioSessions::IsStreaming(uint64_t guid) const noexcept {
    const auto session = Find(guid);
    return session && session->IsStreaming();
}

uint64_t AudioSessions::RunningRun(uint64_t guid) const noexcept {
    const auto session = Find(guid);
    return session ? session->RunningRun() : SessionScheduler::kNotRunning;
}

bool AudioSessions::IsReconciling(uint64_t guid) const noexcept {
    const auto session = Find(guid);
    return session && session->IsReconciling();
}

bool AudioSessions::IsCancelled(uint64_t guid) const noexcept {
    if (TeardownRequested()) {
        return true;
    }
    const auto session = Find(guid);
    return session && session->IsRetired();
}

std::optional<SessionSnapshot> AudioSessions::Snapshot(uint64_t guid) const noexcept {
    const auto session = Find(guid);
    if (!session) {
        return std::nullopt;
    }
    return session->Snapshot();
}

} // namespace ASFW::Audio::Session
