// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeStartAdmission.hpp"
#include <atomic>
namespace Pro40Audio {
// One source thread, one control worker, one owned request/reply slot. Both
// threads must quiesce before destruction. Channel IO occurs only on worker.
class NativeStartMailbox {
public:
    using Bytes=std::array<uint8_t,NativeStartPacket::size>;
    enum class Reply {waiting,ready,failed};
private:
    enum class Phase {empty,queued,taken,replied};
    std::atomic<Phase> phase_{Phase::empty};std::atomic<bool> cancelled_{false};
    Bytes request_{},reply_{};
public:
    void cancel() noexcept {cancelled_.store(true,std::memory_order_release);}
    bool cancelled() const noexcept {return cancelled_.load(std::memory_order_acquire);}
    bool submit(const Bytes &bytes) noexcept {
        if(cancelled()||phase_.load(std::memory_order_acquire)!=Phase::empty)return false;
        request_=bytes;phase_.store(Phase::queued,std::memory_order_release);return true;
    }
    Reply reply(Bytes &out) noexcept {
        if(cancelled())return Reply::failed;
        if(phase_.load(std::memory_order_acquire)!=Phase::replied)return Reply::waiting;
        out=reply_;phase_.store(Phase::empty,std::memory_order_release);return Reply::ready;
    }
    template<class Channel> bool workOnce(Channel &channel) noexcept {
        if(cancelled())return false;
        if(phase_.load(std::memory_order_acquire)!=Phase::queued)return true;
        const auto request=request_;phase_.store(Phase::taken,std::memory_order_release);
        Bytes response{};
        if(!channel.send(request)||cancelled()||!channel.receive(response)){cancel();return false;}
        if(cancelled())return false;
        reply_=response;phase_.store(Phase::replied,std::memory_order_release);return true;
    }
};
// One source tick performs at most one transition. Never waits for worker IO.
// Producer health/admission remain source-owned; no producer reference crosses
// the mailbox. A reply is checked against fresh now, not the request timestamp.
template<class Producer> class NativeStartExchange {
public:
    enum class State {empty,offered,committed,active,failed};
    NativeStartExchange(Producer &p,NativeStartMailbox &m,SourceFrameClock::Identity id,uint64_t nonce)
        :admission_(p,id,nonce),mailbox_(m){}
    ~NativeStartExchange(){if(state_!=State::active)cancel();}
    NativeStartExchange(const NativeStartExchange &)=delete;
    State state() const noexcept {return state_;}
    bool begin(uint64_t now) noexcept {
        NativeStartMailbox::Bytes offer;
        if(state_!=State::empty||!admission_.offer(now,offer)||!mailbox_.submit(offer))return fail();
        state_=State::offered;return true;
    }
    State tick(uint64_t now) noexcept {
        if(state_==State::failed||state_==State::active)return state_;
        if(state_==State::empty||!admission_.poll(now)){fail();return state_;}
        NativeStartMailbox::Bytes reply,request;
        const auto result=mailbox_.reply(reply);
        if(result==NativeStartMailbox::Reply::failed){fail();return state_;}
        if(result==NativeStartMailbox::Reply::waiting)return state_;
        if(state_==State::offered){
            if(!admission_.commit(reply,now,request)||!mailbox_.submit(request)){fail();return state_;}
            state_=State::committed;
        }else{
            if(!admission_.activate(reply,now)){fail();return state_;}
            state_=State::active;
        }
        return state_;
    }
    void cancel() noexcept {fail();}
private:
    bool fail() noexcept {state_=State::failed;mailbox_.cancel();admission_.cancel();return false;}
    NativeSourceAdmission<Producer> admission_;NativeStartMailbox &mailbox_;State state_=State::empty;
};
}
