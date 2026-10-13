// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
namespace Pro40Audio {
// Stable gate allocation outlives ALL callers, including unsuccessful/delayed
// acquisitions. One control owner publishes/closes/reclaims; callback callers
// only acquire and use leases. No callback deletes sessions or waits/retries.
template<class Session> class AudioSessionGate {
    static constexpr uint64_t closed=uint64_t(1)<<63;
    static_assert(sizeof(void*)==8 && std::atomic<uint64_t>::is_always_lock_free);
    // A live lease/caller needs storage in this 64-bit process, so its count
    // cannot reach 2^63. Include transient rejected acquisitions in the count.
    std::atomic<uint64_t> state_{closed};
    std::unique_ptr<Session> session_; // Owner writes only while closed/unpinned.
public:
    class Lease {
        friend class AudioSessionGate;
        AudioSessionGate *gate_;
        explicit Lease(AudioSessionGate *gate):gate_(gate) {}
        void release() noexcept {if(gate_){gate_->state_.fetch_sub(1,std::memory_order_release);gate_=nullptr;}}
    public:
        Lease(const Lease &)=delete;Lease &operator=(const Lease &)=delete;
        Lease(Lease &&other) noexcept:gate_(std::exchange(other.gate_,nullptr)) {}
        Lease &operator=(Lease &&other) noexcept {
            if(this!=&other){release();gate_=std::exchange(other.gate_,nullptr);}return *this;
        }
        ~Lease(){release();}
        // Live lease only. Reference must not escape its lifetime. Session must
        // provide its own reader-safe API; the gate protects lifetime, not writes.
        const Session &get() const noexcept {return *gate_->session_;}
    };
    bool publish(std::unique_ptr<Session> &prepared) noexcept {
        // A null session means the old one was already reclaimed with no
        // admitted pins. Any pins still present now belong to rejected closed
        // acquisitions, which never inspect the session pointer.
        if(!prepared || session_ || !(state_.load(std::memory_order_acquire)&closed))return false;
        session_=std::move(prepared);
        // A rejected closed acquisition can arrive after the check above.
        // Preserve its transient pin so its later decrement cannot underflow.
        state_.fetch_and(~closed,std::memory_order_release);return true;
    }
    std::optional<Lease> tryAcquire() noexcept {
        if(state_.load(std::memory_order_acquire)&closed)return std::nullopt;
        const auto before=state_.fetch_add(1,std::memory_order_acquire);
        if(before&closed){state_.fetch_sub(1,std::memory_order_release);return std::nullopt;}
        // Do not load the pointer before this pin. A caller delayed across a
        // close/reopen can legitimately pin the new complete session instead.
        return Lease(this);
    }
    void closeAdmission() noexcept {state_.fetch_or(closed,std::memory_order_acq_rel);}
    // Serialized control owner only; never a callback acquisition. Publication,
    // close and reclaim cannot race this owner. This avoids rejecting a control
    // Stop merely because a callback changed the pin count during a CAS.
    const Session *ownerSession() const noexcept {return session_.get();}
    std::unique_ptr<Session> tryReclaim() noexcept {
        if(state_.load(std::memory_order_acquire)!=closed)return {};
        return std::move(session_); // Destruction/worker join stays on owner path.
    }
    // Destruction of the gate itself still requires external caller quiescence;
    // a zero pin count alone does not cover callbacks about to tryAcquire().
};
}
