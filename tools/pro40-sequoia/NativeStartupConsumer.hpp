// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeStartExchange.hpp"
#include "NativeTimingEvidence.hpp"
namespace Pro40Audio {
// Source-thread-only adapter. Construct before capture; enable only after worker
// preparation. Packet callbacks ingest content only. Clock/poll callbacks perform
// at most one startup transition, with no waits, thread creation or channel IO.
// Consumer callbacks must themselves meet the bounded native consumer contract.
// Peer failed/activate/cancel must also be nonblocking (NativePeerSession does so).
template<class Consumer,class Peer,class HostClock> class NativeStartupConsumer {
    struct Admission {
        enum class Result {waiting,failed};Consumer &consumer;
        bool prepare(uint64_t now,NativeStartDescriptor &out) noexcept {return consumer.prepareAt(now,out);}
        bool accept(const NativeStartDescriptor &echo,uint64_t now) noexcept {return consumer.acceptAt(echo,now);}
        Result pump(uint64_t now) noexcept {return consumer.pollAt(now)?Result::waiting:Result::failed;}
        void cancel() noexcept {consumer.cancel();}
    } admission_;
    using Exchange=NativeStartExchange<Admission>;
    Consumer &consumer_;Peer &peer_;Pro40::NativeTimingEvidence::Consumer downstream_;
    Exchange exchange_;bool enabled_=false;uint64_t offeredFrames_=0;
    bool advance() noexcept {
        if(!enabled_||peer_.failed()){cancel();return false;}
        if(exchange_.state()==State::empty){
            if(!consumer_.ready())return true;
            offeredFrames_=consumer_.produced();
            return exchange_.begin(HostClock::now());
        }
        if(exchange_.state()==State::active)return true;
        const auto state=exchange_.tick(HostClock::now());
        if(state==State::active)peer_.activate();
        return state!=State::failed;
    }
public:
    using State=typename Exchange::State;
    NativeStartupConsumer(Consumer &consumer,Peer &peer,NativeStartMailbox &mailbox,
                          SourceFrameClock::Identity id,uint64_t nonce)
        :admission_{consumer},consumer_(consumer),peer_(peer),downstream_(consumer.callbacks()),
         exchange_(admission_,mailbox,id,nonce){}
    NativeStartupConsumer(const NativeStartupConsumer &)=delete;
    bool enable() noexcept {
        if(enabled_||exchange_.state()!=State::empty)return false;
        enabled_=true;return true;
    }
    State state() const noexcept {return exchange_.state();}
    uint64_t offeredFrames() const noexcept {return offeredFrames_;}
    void cancel() noexcept {exchange_.cancel();peer_.cancel();}
    Pro40::NativeTimingEvidence::Consumer callbacks() noexcept {
        return {this,[](void *p,size_t n,const auto &r) noexcept {
            auto &self=*static_cast<NativeStartupConsumer *>(p);
            if(!self.enabled_||!self.downstream_.record(self.downstream_.context,n,r)){self.cancel();return false;}
            return true;
        },[](void *p,const auto &r) noexcept {
            auto &self=*static_cast<NativeStartupConsumer *>(p);
            if(!self.enabled_||!self.downstream_.clock(self.downstream_.context,r)){self.cancel();return false;}
            return self.advance();
        },[](void *p) noexcept {
            auto &self=*static_cast<NativeStartupConsumer *>(p);
            if(!self.enabled_||!self.downstream_.poll(self.downstream_.context)){self.cancel();return false;}
            return self.advance();
        }};
    }
};
}
