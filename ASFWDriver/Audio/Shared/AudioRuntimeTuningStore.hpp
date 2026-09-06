// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "AudioRuntimeTuning.hpp"
#include <expected>
#include <limits>
#include <optional>
#include <utility>
#ifdef ASFW_HOST_TEST
#include <mutex>
#else
#include <DriverKit/IOLib.h>
#endif

namespace ASFW::Audio::Shared {

inline constexpr uint32_t kSupportedTuningGroups =
    static_cast<uint32_t>(TuningGroup::kTransmitDepth);
inline constexpr uint64_t kRuntimeTuningTokenPrefix = 0xA5F7000000000000ULL;
[[nodiscard]] constexpr uint64_t TuningToken(uint32_t requestId) noexcept {
    return kRuntimeTuningTokenPrefix | requestId;
}
[[nodiscard]] constexpr bool IsTuningToken(uint64_t token) noexcept {
    return (token & 0xffffffff00000000ULL) == kRuntimeTuningTokenPrefix;
}

enum class TuningRequestStatus : uint32_t {
    Idle, Queued, Requested, Applying, Applied, Rejected, Aborted, Unchanged
};

struct RuntimeTuningSnapshot {
    AudioRuntimeTuning effective{};
    uint32_t sampleRateHz{0}, inputChannels{0}, outputChannels{0};
    uint32_t requestId{0}, appliedSequence{0}, pendingGroups{0};
    uint32_t lastError{0}, warnings{0};
    TuningRequestStatus status{TuningRequestStatus::Idle};
    bool ready{false}, streaming{false};
};

// All operations run under RuntimeTuningStore's lock, including the action
// ownership maintained by the nub. No action dispatch or ADK call holds it.
class RuntimeTuningState final {
public:
    [[nodiscard]] RuntimeTuningSnapshot Copy() const noexcept { return snapshot_; }

    void PublishGraph(const AudioRuntimeTuning& effective, uint32_t rate,
                      uint32_t inputs, uint32_t outputs) noexcept {
        snapshot_.effective = effective;
        snapshot_.sampleRateHz = rate;
        snapshot_.inputChannels = inputs;
        snapshot_.outputChannels = outputs;
        snapshot_.ready = true;
    }

    void SetStreaming(bool running, uint32_t rate = 0) noexcept {
        snapshot_.streaming = running;
        if (rate != 0) snapshot_.sampleRateHz = rate;
    }

    void Disconnect(uint32_t error) noexcept {
        if (Busy()) (void)Finish(snapshot_.requestId, error, true);
        snapshot_.ready = false;
        snapshot_.streaming = false;
    }

    [[nodiscard]] std::expected<uint32_t, TuningRejection> Submit(
        const AudioRuntimeTuning& request, uint32_t groups) noexcept {
        if (!snapshot_.ready) return std::unexpected(TuningRejection::kNotReady);
        if (groups & ~kSupportedTuningGroups)
            return std::unexpected(TuningRejection::kUnsupportedGroups);
        if (Busy()) return std::unexpected(TuningRejection::kBusy);
        auto candidate = snapshot_.effective;
        // Only supported, selected fields are copied. Stale/unselected fields
        // in a wire request cannot roll back another setting or fail validation.
        if (Contains(groups, TuningGroup::kTransmitDepth)) {
            candidate.txDispatchSlackPackets = request.txDispatchSlackPackets;
            candidate.txOwnershipGuardPackets = request.txOwnershipGuardPackets;
        }
        const auto validation = ValidateTuning(candidate);
        if (!validation.Applicable()) return std::unexpected(validation.rejection);
        if (CostOf(groups, candidate, snapshot_.effective) == ApplyCost::kNothing) {
            snapshot_.status = TuningRequestStatus::Unchanged;
            snapshot_.lastError = 0;
            return 0;
        }
        if (snapshot_.requestId == std::numeric_limits<uint32_t>::max())
            return std::unexpected(TuningRejection::kRequestIdExhausted);
        ++snapshot_.requestId;
        candidate_ = candidate;
        candidateWarnings_ = validation.warnings;
        snapshot_.pendingGroups = groups;
        snapshot_.lastError = 0;
        snapshot_.status = TuningRequestStatus::Queued;
        return snapshot_.requestId;
    }

    [[nodiscard]] bool RequestWindow(uint32_t id) noexcept {
        if (id != snapshot_.requestId || snapshot_.status != TuningRequestStatus::Queued)
            return false;
        snapshot_.status = TuningRequestStatus::Requested;
        return true;
    }

    [[nodiscard]] std::optional<AudioRuntimeTuning> BeginApply(uint32_t id) noexcept {
        if (id != snapshot_.requestId || snapshot_.status != TuningRequestStatus::Requested)
            return std::nullopt;
        snapshot_.status = TuningRequestStatus::Applying;
        return candidate_;
    }

    [[nodiscard]] bool Finish(uint32_t id, uint32_t error, bool aborted = false) noexcept {
        if (id != snapshot_.requestId || !Busy()) return false;
        if (error == 0 && !aborted) {
            if (snapshot_.status != TuningRequestStatus::Applying) return false;
            snapshot_.effective = candidate_;
            snapshot_.warnings = candidateWarnings_;
            ++snapshot_.appliedSequence;
            snapshot_.status = TuningRequestStatus::Applied;
        } else {
            snapshot_.status = aborted ? TuningRequestStatus::Aborted
                                       : TuningRequestStatus::Rejected;
        }
        snapshot_.lastError = error;
        snapshot_.pendingGroups = 0;
        return true;
    }

private:
    [[nodiscard]] bool Busy() const noexcept {
        return snapshot_.pendingGroups != 0;
    }
    RuntimeTuningSnapshot snapshot_{};
    AudioRuntimeTuning candidate_{};
    uint32_t candidateWarnings_{0};
};

// Same locked transaction used by the production nub and the threaded host
// tests. IOLock is appropriate here: no operation is on a real-time audio path.
struct RuntimeTuningStore final {
public:
    RuntimeTuningStore() noexcept = default;
    RuntimeTuningStore(const RuntimeTuningStore&) = delete;
    RuntimeTuningStore& operator=(const RuntimeTuningStore&) = delete;
    ~RuntimeTuningStore() {
#ifndef ASFW_HOST_TEST
        if (lock_) IOLockFree(lock_);
#endif
    }
    [[nodiscard]] bool IsValid() const noexcept {
#ifdef ASFW_HOST_TEST
        return true;
#else
        return lock_ != nullptr;
#endif
    }
    template <typename F>
    auto WithLock(F&& operation) {
        struct Guard {
            RuntimeTuningStore& store;
            explicit Guard(RuntimeTuningStore& s) : store(s) { store.Lock(); }
            ~Guard() { store.Unlock(); }
        } guard{*this};
        return std::forward<F>(operation)(state_);
    }
private:
    void Lock() {
#ifdef ASFW_HOST_TEST
        lock_.lock();
#else
        IOLockLock(lock_);
#endif
    }
    void Unlock() {
#ifdef ASFW_HOST_TEST
        lock_.unlock();
#else
        IOLockUnlock(lock_);
#endif
    }
#ifdef ASFW_HOST_TEST
    std::mutex lock_;
#else
    IOLock* lock_{IOLockAlloc()};
#endif
    RuntimeTuningState state_;
};
} // namespace ASFW::Audio::Shared
