// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeTimingEvidence.hpp"
#include "NativeFiniteInputSession.hpp"
#include "RecordedInputSession.hpp"
#include "NativeInputProducer.hpp"
#include "NativeProducerExchange.hpp"
namespace Pro40Audio {
// Source-thread-only content adapter. Construct storage before native entry.
// HostClock::now() is live host ticks (or explicit saved ticks in tests).
// Startup prepare/accept and finish are driven by serialized supervision, never
// by blocking IPC in these callbacks. Mailbox/recorder outlive this object;
// sender must be joined before destruction. No hardware or endpoint ownership.
template<class HostClock> class NativeProducerConsumer {
    using Owner=NativeFiniteInputSession<8192>;
    using Producer=NativeInputProducer<Owner>;
    using Exchange=NativeProducerExchange<Producer>;
    const Pro40Packets::InputRecording &recording_;NativeFeedMailbox &mailbox_;
    SourceFrameClock::Identity id_;uint64_t pendingAgeNs_;bool failed_=false,ended_=false;
    std::optional<Owner> owner_;
    std::optional<RecordedInputSession<8192,Owner>> adapter_;
    std::optional<Producer> producer_;
    std::optional<Exchange> exchange_;
    uint64_t ticks(uint64_t ns) const noexcept {return uint64_t(__uint128_t(ns)*id_.denom/id_.numer);}
    bool fail() noexcept {
        failed_=true;mailbox_.cancel();if(exchange_)exchange_->cancel();if(adapter_)adapter_->failed();return false;
    }
    bool packet(size_t ordinal,const Pro40Packets::InputTimingEvidence<>::Record &row) noexcept {
        return !failed_&&adapter_&&adapter_->record(ordinal,row,HostClock::now())?true:fail();
    }
    bool clock(const Pro40::NativeTimingEvidence::Clock &p) noexcept {
        if(failed_)return false;
        if(!owner_){
            const auto duration=ticks(4000000000);
            if(!p.beforeReceive||p.generation!=id_.generation||p.before>UINT64_MAX-duration)return fail();
            owner_.emplace(id_,SourceClockReadiness::Policy{4096,512,8192,ticks(50000000)},2880000,
                p.before,p.before+duration,ticks(50000000),ticks(100000000),
                typename Owner::Input::TimingPolicy{ticks(1000000000),ticks(250000000)},ticks(1000),32);
            adapter_.emplace(*owner_,recording_,id_.epoch);producer_.emplace(*owner_,ticks(pendingAgeNs_));
            exchange_.emplace(*producer_,mailbox_,id_.epoch,ticks(100000000));
        }
        if(!adapter_->clock({p.before,p.uptime,p.after,p.generation,p.cycle,p.status,p.packetCount,p.beforeReceive},HostClock::now()))return fail();
        return poll();
    }
public:
    NativeProducerConsumer(const Pro40Packets::InputRecording &recording,NativeFeedMailbox &mailbox,SourceFrameClock::Identity id,
                           uint64_t pendingAgeNs=100000000)
        :recording_(recording),mailbox_(mailbox),id_(id),pendingAgeNs_(pendingAgeNs){
        if(!id.epoch||!id.seed||!id.numer||!id.denom||!pendingAgeNs||pendingAgeNs>1000000000||!recording.valid()||recording.fault()||!recording.pcm().empty())fail();
    }
    NativeProducerConsumer(const NativeProducerConsumer &)=delete;
    Pro40::NativeTimingEvidence::Consumer callbacks() noexcept {
        return {this,[](void *p,size_t n,const auto &r) noexcept {return static_cast<NativeProducerConsumer *>(p)->packet(n,r);},
            [](void *p,const auto &r) noexcept {return static_cast<NativeProducerConsumer *>(p)->clock(r);},
            [](void *p) noexcept {return static_cast<NativeProducerConsumer *>(p)->poll();}};
    }
    bool poll() noexcept {return pollAt(HostClock::now());}
    // Admission must use one fresh timestamp for health, descriptor preparation
    // and negotiation; resampling here could make preparedHost newer than now.
    bool pollAt(uint64_t now) noexcept {
        if(failed_)return false;
        if(!adapter_)return true;
        if(!adapter_->poll(now))return fail();
        const auto result=exchange_->tick(now);
        if(result==Exchange::Result::failed)return fail();
        ended_=result==Exchange::Result::ended;
        return true;
    }
    bool ready() const noexcept {return !failed_&&owner_&&owner_->state()==Owner::State::ready;}
    bool prepare(NativeStartDescriptor &out) noexcept {return prepareAt(HostClock::now(),out);}
    bool prepareAt(uint64_t now,NativeStartDescriptor &out) noexcept {
        return pollAt(now)&&producer_&&producer_->prepare(now,out)?true:fail();
    }
    bool accept(const NativeStartDescriptor &echo) noexcept {return acceptAt(echo,HostClock::now());}
    bool acceptAt(const NativeStartDescriptor &echo,uint64_t now) noexcept {
        return pollAt(now)&&producer_&&producer_->accept(echo,now)?true:fail();
    }
    bool finish(bool sourceAccepted) noexcept {
        return poll()&&producer_&&producer_->finish(recording_.pcm().size()/3,HostClock::now(),sourceAccepted)?true:fail();
    }
    void cancel() noexcept {fail();}
    bool failed() const noexcept {return failed_;}
    bool ended() const noexcept {return !failed_&&ended_;}
    uint64_t delivered() const noexcept {return producer_?producer_->delivered():0;}
    uint64_t nextRead() const noexcept {return owner_?owner_->nextRead():0;}
    uint64_t produced() const noexcept {return owner_?owner_->produced():0;}
    // Diagnostic value only; does not poll, refresh or authorize the clock.
    std::optional<typename Owner::ClockSelection> selectedClockEvidence()const noexcept {
        return owner_?owner_->selectedClockEvidence():std::nullopt;
    }
};
}
