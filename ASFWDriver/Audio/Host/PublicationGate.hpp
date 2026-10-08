// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// PublicationGate.hpp
// Combined atomic admission gate synchronizing device/nub publication with
// driver teardown. Admission check and in-flight tracking are performed
// in a single atomic CAS so teardown and publication cannot race.

#pragma once

#include <DriverKit/IOLib.h>
#include <atomic>
#include <cstdint>
#include <functional>

namespace ASFW::Audio {

class PublicationGate {
public:
    static constexpr int32_t kClosedSentinel = 1'000'000;

    /// RAII guard representing an active publication admission.
    class AdmissionScope {
    public:
        AdmissionScope() noexcept = default;

        explicit AdmissionScope(PublicationGate& gate) noexcept
            : gate_(&gate) {
            admitted_ = gate_->TryEnter();
        }

        ~AdmissionScope() noexcept {
            ExitIfAdmitted();
        }

        AdmissionScope(const AdmissionScope&) = delete;
        AdmissionScope& operator=(const AdmissionScope&) = delete;

        AdmissionScope(AdmissionScope&& other) noexcept
            : gate_(other.gate_), admitted_(other.admitted_) {
            other.gate_ = nullptr;
            other.admitted_ = false;
        }

        AdmissionScope& operator=(AdmissionScope&& other) noexcept {
            if (this != &other) {
                ExitIfAdmitted();
                gate_ = other.gate_;
                admitted_ = other.admitted_;
                other.gate_ = nullptr;
                other.admitted_ = false;
            }
            return *this;
        }

        [[nodiscard]] bool IsAdmitted() const noexcept { return admitted_; }

        /// Returns true if teardown closed the gate while this admission was in flight.
        [[nodiscard]] bool IsStopping() const noexcept {
            return gate_ && gate_->IsClosed();
        }

        /// Aborts the admission if stopping, records the rejection, and immediately
        /// exits the gate to unblock teardown.
        [[nodiscard]] bool AbortIfStopping() noexcept {
            if (IsStopping()) {
                if (admitted_) {
                    admitted_ = false;
                    gate_->RecordReject();
                    gate_->Exit();
                }
                return true;
            }
            return false;
        }

    private:
        void ExitIfAdmitted() noexcept {
            if (admitted_ && gate_) {
                admitted_ = false;
                gate_->Exit();
            }
        }

        PublicationGate* gate_{nullptr};
        bool admitted_{false};
    };

    PublicationGate() noexcept = default;

    /// Atomically admits one publication if gate is open (state >= 0).
    [[nodiscard]] bool TryEnter() noexcept {
        int32_t cur = state_.load(std::memory_order_relaxed);
        while (cur >= 0) {
            if (state_.compare_exchange_weak(cur, cur + 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
                return true;
            }
        }
        rejectCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    /// Exits one active admission.
    void Exit() noexcept {
        state_.fetch_sub(1, std::memory_order_acq_rel);
    }

    /// Returns true if teardown has closed admission.
    [[nodiscard]] bool IsClosed() const noexcept {
        return state_.load(std::memory_order_acquire) < 0;
    }

    void RecordReject() noexcept {
        rejectCount_.fetch_add(1, std::memory_order_relaxed);
    }

    /// Atomically closes admission so no future TryEnter can succeed,
    /// and waits synchronously until all in-flight admissions exit.
    void CloseAndWait() noexcept {
        int32_t cur = state_.load(std::memory_order_relaxed);
        while (cur >= 0) {
            if (state_.compare_exchange_weak(cur, cur - kClosedSentinel,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
                break;
            }
        }

#ifdef ASFW_HOST_TEST
        if (onGateClosedForTesting_) {
            onGateClosedForTesting_();
        }
#endif

        while (state_.load(std::memory_order_acquire) != -kClosedSentinel) {
            IOSleep(1);
        }
    }

    [[nodiscard]] uint64_t RejectCount() const noexcept {
        return rejectCount_.load(std::memory_order_relaxed);
    }

#ifdef ASFW_HOST_TEST
    void SetOnGateClosedForTesting(std::function<void()> hook) noexcept {
        onGateClosedForTesting_ = std::move(hook);
    }
#endif

private:
    std::atomic<int32_t> state_{0};
    std::atomic<uint64_t> rejectCount_{0};
#ifdef ASFW_HOST_TEST
    std::function<void()> onGateClosedForTesting_{};
#endif
};

} // namespace ASFW::Audio
