// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFiniteFeed.hpp"
#include "AudioFeedControl.hpp"
#include <chrono>
namespace Pro40Audio {
// Local worker adapter. Fixed 250ms drain budget begins when END is received.
// No producer ACK is accepted; completion uses a separate bounded mailbox.
template<size_t Capacity> class AudioFiniteWorkerSession {
    AudioFiniteFeed<Capacity> finite_;
    const uint64_t epoch_;
    std::atomic<uint64_t> expectedAck_{0},ack_{0};
    SessionResult terminal_=SessionResult::ok;
    uint64_t accepted_=0,published_=0;
    std::chrono::steady_clock::time_point drainDeadline_{};
    bool pending_=false;
    static uint64_t now() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
public:
    static constexpr size_t maxPacketBytes=AudioFeedPacket::maxBytes;
    AudioFiniteWorkerSession(uint64_t epoch,uint64_t first):finite_(epoch),epoch_(epoch) {
        if(first){terminal_=SessionResult::malformed;finite_.cancel();}
    }
    auto tryAcquire() noexcept {return finite_.tryAcquire();}
    auto readInput(uint64_t epoch,uint64_t first,std::span<int32_t> out) noexcept {
        auto pin=finite_.tryAcquire();
        if(!pin){std::fill(out.begin(),out.end(),0);return typename AudioFiniteFeed<Capacity>::ReadResult{};}
        return finite_.read(*pin,epoch,first,out);
    }
    bool closed() const {return finite_.state()==AudioFiniteFeed<Capacity>::State::closed;}
    bool pending() const {return pending_;}
    SessionResult terminal() const {return terminal_;}
    uint64_t acceptedNext() const {return accepted_;}
    uint64_t publishedNext() const {return published_;}
    void close() {expectedAck_.store(0,std::memory_order_release);finite_.cancel();pending_=false;}
    auto pollDeadline(std::chrono::steady_clock::time_point proposed) const {
        return finite_.state()==AudioFiniteFeed<Capacity>::State::draining?std::min(proposed,drainDeadline_):proposed;
    }
    SessionResult flush() {
        const auto r=finite_.flush();pending_=r==SessionResult::pending;
        if(r==SessionResult::ok)published_=accepted_;else if(!pending_)terminal_=r;
        return r;
    }
    SessionResult receive(std::span<const uint8_t> bytes) {
        if(bytes.size()==AudioFeedControl::size) {
            AudioFeedControl c;const auto t=now();
            if(!AudioFeedControl::decode(bytes,c)||c.kind!=AudioFeedControl::Kind::end||
               !finite_.finish(c.epoch,c.frame,t,t+250000000)) {
                terminal_=SessionResult::malformed;close();return terminal_;
            }
            drainDeadline_=std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(t+250000000)));
            expectedAck_.store(((c.frame+511)/512)*512,std::memory_order_release);
            return SessionResult::ok;
        }
        AudioFeedBatch b;
        if(AudioFeedPacket::decode(bytes,b)!=AudioFeedError::ok){terminal_=SessionResult::malformed;close();return terminal_;}
        const auto r=finite_.receive(bytes);
        if(r==SessionResult::ok || r==SessionResult::pending) {
            accepted_=b.firstFrame+b.frames;pending_=r==SessionResult::pending;
            if(!pending_)published_=accepted_;
        } else terminal_=r;
        return r;
    }
    // Bounded callback-completion submission. Caller must have finished reading;
    // this is not proof of callback execution and not multi-client accounting.
    bool postDrainAck(std::span<const uint8_t> bytes) noexcept {
        AudioFeedControl c;
        if(!AudioFeedControl::decode(bytes,c)||c.kind!=AudioFeedControl::Kind::ack||c.epoch!=epoch_||
           c.frame!=expectedAck_.load(std::memory_order_acquire))return false;
        uint64_t zero=0;return ack_.compare_exchange_strong(zero,c.frame,std::memory_order_release,std::memory_order_relaxed);
    }
    FeedLifecycle pollLifecycle() {
        const auto t=now();finite_.poll(t);
        const auto ack=ack_.load(std::memory_order_acquire);
        if(ack)finite_.acknowledge(epoch_,ack,t);
        using Reason=typename AudioFiniteFeed<Capacity>::EndReason;
        switch(finite_.reason()) {
        case Reason::acknowledged:return FeedLifecycle::drained;
        case Reason::timeout:return FeedLifecycle::timedOut;
        case Reason::none:return FeedLifecycle::active;
        default:return FeedLifecycle::fault;
        }
    }
};
}
