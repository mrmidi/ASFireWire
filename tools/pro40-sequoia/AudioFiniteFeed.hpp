// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFeedSession.hpp"

namespace Pro40Audio {
// Local lab only: one control owner, readers use leases, one logical drain
// acknowledgement. No IPC authentication or multi-client completion tracking.
// Close never frees storage. Quiesce ALL callers and drop leases before destroy.
template<size_t Capacity> class AudioFiniteFeed {
    AudioFeedSession<Capacity> feed_;
    const uint64_t epoch_;
    std::atomic<uint64_t> final_{0}; // Release only after the final PCM publication.
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
public:
    enum class State { streaming, draining, closed };
    enum class EndReason { none, acknowledged, timeout, cancelled, invalid };
    struct ReadResult {
        WindowResult result=WindowResult::invalidRange;
        size_t audioFrames=0,paddingFrames=0;
        bool reachedEnd=false;
        bool ok() const {return result==WindowResult::ok;}
    };
private:
    State state_=State::streaming;
    EndReason reason_=EndReason::none;
    uint64_t deadline_=0,lastTime_=0,ackThrough_=0;
    void close(EndReason reason) {
        if(state_==State::closed)return;
        feed_.close();state_=State::closed;reason_=reason;
    }
    bool time(uint64_t now) {
        if(now<lastTime_){close(EndReason::invalid);return false;}
        lastTime_=now;return true;
    }
public:
    explicit AudioFiniteFeed(uint64_t epoch):feed_(epoch,0),epoch_(epoch) {
        if(!epoch)close(EndReason::invalid);
    }
    State state() const {return state_;} // Control-owner only.
    EndReason reason() const {return reason_;}
    auto tryAcquire() noexcept {return feed_.tryAcquire();}
    SessionResult receive(std::span<const uint8_t> bytes) {
        if(state_!=State::streaming){close(EndReason::invalid);return SessionResult::closed;}
        const auto result=feed_.receive(bytes);
        if(feed_.closed())close(EndReason::invalid);
        return result;
    }
    SessionResult flush() {
        if(state_!=State::streaming)return SessionResult::closed;
        const auto result=feed_.flush();
        if(feed_.closed())close(EndReason::invalid);
        return result;
    }
    bool finish(uint64_t epoch,uint64_t finalFrame,uint64_t now,uint64_t deadline) {
        if(!time(now))return false;
        if(state_!=State::streaming || epoch!=epoch_ || !finalFrame || finalFrame%8 ||
           finalFrame>UINT64_MAX-511 || deadline<=now || feed_.pending() ||
           feed_.acceptedNext()!=finalFrame || feed_.publishedNext()!=finalFrame) {
            close(EndReason::invalid);return false;
        }
        deadline_=deadline;ackThrough_=((finalFrame+511)/512)*512;
        state_=State::draining;final_.store(finalFrame,std::memory_order_release);return true;
    }
    void poll(uint64_t now) {
        if(state_==State::closed || !time(now))return;
        if(state_==State::draining && now>=deadline_)close(EndReason::timeout);
    }
    // Trusted local owner reports completion AFTER its final callback returns.
    // An acknowledgement closes admission; it never authorizes destruction.
    bool acknowledge(uint64_t epoch,uint64_t through,uint64_t now) {
        poll(now);
        if(state_!=State::draining)return false;
        if(epoch!=epoch_ || through!=ackThrough_){close(EndReason::invalid);return false;}
        close(EndReason::acknowledged);return true;
    }
    void cancel() {close(EndReason::cancelled);}
    template<class Lease>
    ReadResult read(const Lease &lease,uint64_t epoch,uint64_t first,std::span<int32_t> output) const noexcept {
        std::fill(output.begin(),output.end(),0);
        if(epoch!=epoch_)return {WindowResult::wrongEpoch};
        if(output.empty() || output.size()>Capacity || first>UINT64_MAX-output.size())return {};
        const auto end=final_.load(std::memory_order_acquire);
        if(end && first>=end)return {};
        const size_t count=end?size_t(std::min<uint64_t>(output.size(),end-first)):output.size();
        const auto result=lease.read(epoch,first,output.first(count));
        if(result!=WindowResult::ok)return {result};
        return {result,count,output.size()-count,end && first+count==end};
    }
};
}
