// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFeedPacket.hpp"
#include <atomic>
namespace Pro40Audio {
// Process-local single-slot handoff: exactly one source thread and one sender
// thread for its whole lifetime. Quiesce BOTH before destruction; no reset/reuse.
// No native owner reference crosses this boundary. Cancellation can race with
// an in-flight send; it cannot retract bytes already accepted by the transport.
class NativeFeedMailbox {
public:
    struct Message {uint64_t sequence=0;bool end=false;AudioFeedBatch batch;};
    enum class Receipt {waiting,accepted,failed};
private:
    enum class Phase {empty,queued,taken,accepted};
    static_assert(std::atomic<Phase>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    std::atomic<Phase> phase_{Phase::empty};
    std::atomic<bool> cancelled_{false};
    Message slot_{};
    uint64_t next_=1;bool ended_=false; // Source thread only.
    uint64_t inFlight_=0;             // Sender thread only.
public:
    void cancel() noexcept {cancelled_.store(true,std::memory_order_release);}
    bool cancelled()const noexcept{return cancelled_.load(std::memory_order_acquire);}
    // Source side. END carries epoch and exact exclusive end in firstFrame.
    bool submit(const AudioFeedBatch &batch,bool end=false) noexcept {
        if(cancelled()||ended_||!next_||phase_.load(std::memory_order_acquire)!=Phase::empty)return false;
        if(!batch.epoch||(end?(!batch.firstFrame||batch.firstFrame%8||batch.frames):
           (!batch.frames||batch.frames>512||batch.frames%8||batch.firstFrame>UINT64_MAX-batch.frames))){cancel();return false;}
        if(!end)for(uint32_t i=0;i<batch.frames;++i)if(batch.samples[i]<-8388608||batch.samples[i]>8388607){cancel();return false;}
        slot_={next_++,end,batch};ended_=end;
        phase_.store(Phase::queued,std::memory_order_release);return true;
    }
    Receipt receipt(Message &out) noexcept {
        if(cancelled())return Receipt::failed;
        if(phase_.load(std::memory_order_acquire)!=Phase::accepted)return Receipt::waiting;
        out=slot_;phase_.store(Phase::empty,std::memory_order_release);return Receipt::accepted;
    }
    // Sender side. Copy out before sending. Exactly one completion per take;
    // acknowledgement means transport accepted it, not that HAL consumed it.
    bool take(Message &out) noexcept {
        if(cancelled()||inFlight_||phase_.load(std::memory_order_acquire)!=Phase::queued)return false;
        out=slot_;inFlight_=out.sequence;
        phase_.store(Phase::taken,std::memory_order_release);return true;
    }
    bool complete(uint64_t sequence,bool accepted) noexcept {
        if(cancelled()||!accepted||!inFlight_||sequence!=inFlight_){cancel();return false;}
        inFlight_=0;phase_.store(Phase::accepted,std::memory_order_release);return true;
    }
};
}
