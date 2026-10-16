// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
namespace Pro40 {
// One gate for the singleton native backend. Declare Lease before all native
// locals so they are destroyed before another caller can enter.
class SessionAdmission {
    enum class State {idle, active, blocked};
    std::atomic<State> state_{State::idle};
public:
    class Lease {
        SessionAdmission &gate_;
        bool held_=false, blocked_=false;
    public:
        explicit Lease(SessionAdmission &gate) noexcept : gate_(gate) {
            auto expected=State::idle;
            held_=gate_.state_.compare_exchange_strong(expected,State::active);
        }
        Lease(const Lease &)=delete;
        Lease &operator=(const Lease &)=delete;
        explicit operator bool() const noexcept {return held_;}
        void blockRestart() noexcept {if(held_)blocked_=true;}
        ~Lease() {
            if(held_)gate_.state_.store(blocked_?State::blocked:State::idle);
        }
    };
};
}
