// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../Model/ASFWAudioDevice.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <cstdint>

namespace ASFW::Audio {

class AudioEndpointRuntime final : public Runtime::IDirectAudioBindingSource {
public:
    explicit AudioEndpointRuntime(uint64_t guid) noexcept : guid_(guid), lock_(IOLockAlloc()) {}
    ~AudioEndpointRuntime() noexcept {
        if (lock_) {
            IOLockFree(lock_);
            lock_ = nullptr;
        }
    }

    AudioEndpointRuntime(const AudioEndpointRuntime&) = delete;
    AudioEndpointRuntime& operator=(const AudioEndpointRuntime&) = delete;

    [[nodiscard]] uint64_t Guid() const noexcept { return guid_; }

    void UpdateConfig(const Model::ASFWAudioDevice& config) noexcept {
        if (lock_) {
            IOLockLock(lock_);
        }
        config_ = config;
        if (lock_) {
            IOLockUnlock(lock_);
        }
        configValid_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool CopyConfig(Model::ASFWAudioDevice& outConfig) const noexcept {
        if (!configValid_.load(std::memory_order_acquire)) {
            return false;
        }
        if (lock_) {
            IOLockLock(lock_);
        }
        outConfig = config_;
        if (lock_) {
            IOLockUnlock(lock_);
        }
        return true;
    }

    void PublishDirectAudioBinding(const Runtime::DirectAudioBindingSnapshot& snapshot) noexcept {
        if (lock_) {
            IOLockLock(lock_);
        }
        binding_ = snapshot;
        binding_.valid = snapshot.IsValidDuplex();
        bindingGeneration_.store(snapshot.generation, std::memory_order_release);
        if (lock_) {
            IOLockUnlock(lock_);
        }
    }

    void ClearDirectAudioBinding() noexcept {
        if (lock_) {
            IOLockLock(lock_);
        }
        binding_ = {};
        bindingGeneration_.fetch_add(1, std::memory_order_acq_rel);
        if (lock_) {
            IOLockUnlock(lock_);
        }
    }

    bool CopyDirectAudioBinding(Runtime::DirectAudioBindingSnapshot& out) noexcept override {
        if (lock_) {
            IOLockLock(lock_);
        }
        if (!binding_.IsValidDuplex()) {
            out = {};
            if (lock_) {
                IOLockUnlock(lock_);
            }
            return false;
        }
        out = binding_;
        out.generation = bindingGeneration_.load(std::memory_order_acquire);
        if (lock_) {
            IOLockUnlock(lock_);
        }
        return true;
    }

    void MarkStreaming(bool streaming) noexcept {
        streaming_.store(streaming, std::memory_order_release);
    }

    [[nodiscard]] bool IsStreaming() const noexcept {
        return streaming_.load(std::memory_order_acquire);
    }

private:
    uint64_t guid_{0};
    mutable IOLock* lock_{nullptr};
    Model::ASFWAudioDevice config_{};
    std::atomic<bool> configValid_{false};
    Runtime::DirectAudioBindingSnapshot binding_{};
    std::atomic<uint64_t> bindingGeneration_{0};
    std::atomic<bool> streaming_{false};
};

} // namespace ASFW::Audio
