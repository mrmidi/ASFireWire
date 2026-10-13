// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeStartExchange.hpp"
#include <chrono>
#include <thread>
#include <utility>
namespace Pro40Audio {
// Single-use non-RT thread owner. Source admission/producer stays with caller.
// Channel, mailbox and continuation captures must outlive join. Channel calls
// and continuation must be bounded/cooperatively cancellable by their owners.
// No endpoint is closed here: a failed join retains ownership, and destruction
// while joinable terminates via std::thread rather than detaching live resources.
class NativeStartWorker {
public:
    using Wall=std::chrono::steady_clock;
    explicit NativeStartWorker(NativeStartMailbox &mailbox):mailbox_(mailbox){}
    NativeStartWorker(const NativeStartWorker &)=delete;
    NativeStartWorker &operator=(const NativeStartWorker &)=delete;
    // Channel implements send/receive. Continuation runs only after source
    // activates; it owns capability setup and subsequent data IO on this thread.
    // false reports startup failure; data-phase failures use their own contract.
    // Deadline covers preparation-to-activation, including waiting for OFFER.
    // Source readiness must not restart it and leave an idle peer alive forever.
    template<class Channel,class Continuation>
    bool start(Channel &channel,Wall::time_point deadline,Continuation continuation){
        if(started_||mailbox_.cancelled())return false;
        started_=true;
        thread_=std::thread([this,&channel,deadline,next=std::move(continuation)]() mutable {
            for(;;){
                // Recheck after channel IO too: activation cannot bypass expiry.
                if(Wall::now()>=deadline||mailbox_.cancelled()){finish(false);return;}
                if(activated_.load(std::memory_order_acquire))break;
                if(!mailbox_.workOnce(channel)){finish(false);return;}
                pauseUntil(deadline);
            }
            if(mailbox_.cancelled()){finish(false);return;}
            finish(next());
        });
        return true;
    }
    // Caller invokes only after source admission accepts READY.
    void activate() noexcept {activated_.store(true,std::memory_order_release);}
    void cancel() noexcept {mailbox_.cancel();}
    bool failed() const noexcept {return failed_.load(std::memory_order_acquire);}
    bool done() const noexcept {return done_.load(std::memory_order_acquire);}
    bool joinable() const noexcept {return thread_.joinable();} // Supervisor only.
    // Never call on the source's real-time callback. Timeout does NOT release
    // captures, ports or mailbox; caller must retain them until a later join.
    bool joinUntil(Wall::time_point deadline){
        if(!thread_.joinable())return true;
        while(!done()){
            if(Wall::now()>=deadline)return false;
            pauseUntil(deadline);
        }
        thread_.join();return true;
    }
private:
    // Non-RT polling only. No source-side lock/notification required. Scheduling
    // may oversleep; callers must treat this as pacing, not a latency guarantee.
    static void pauseUntil(Wall::time_point deadline){
        const auto next=Wall::now()+std::chrono::microseconds(100);
        std::this_thread::sleep_until(next<deadline?next:deadline);
    }
    void finish(bool ok) noexcept {
        if(!ok){mailbox_.cancel();failed_.store(true,std::memory_order_release);}
        done_.store(true,std::memory_order_release);
    }
    NativeStartMailbox &mailbox_;std::thread thread_;bool started_=false;
    std::atomic<bool> activated_{false},failed_{false},done_{false};
};
}
