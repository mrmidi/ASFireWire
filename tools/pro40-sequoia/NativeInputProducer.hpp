// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PreparedNativeStart.hpp"
#include "AudioFeedPacket.hpp"
namespace Pro40Audio {
// Serialized content-side adapter, NOT a concurrent queue or transport worker.
// Owner outlives this object. All owner ingestion and these methods must run on
// the same serialized control path. A future IPC worker receives copied batches,
// never this reference. stop() terminates the content owner; hardware cleanup
// remains the supervisor's responsibility. No allocation, waiting or IO here.
template<class Owner> class NativeInputProducer {
public:
    enum class State {empty,prepared,streaming,ended,failed};
    enum class Result {waiting,batch,end,failed};
    NativeInputProducer(Owner &owner,uint64_t pendingAge):owner_(owner),pendingAge_(pendingAge){
        if(!pendingAge_)fail();
    }
    ~NativeInputProducer(){if(state_!=State::ended&&state_!=State::failed)fail();}
    NativeInputProducer(const NativeInputProducer &)=delete;
    NativeInputProducer &operator=(const NativeInputProducer &)=delete;
    State state()const noexcept{return state_;}
    uint64_t delivered()const noexcept{return delivered_;}
    bool prepare(uint64_t now,NativeStartDescriptor &out) noexcept {
        if(state_!=State::empty||!health(now)||!start_.prepare(owner_,now)||!start_.proposal(now,descriptor_))return fail();
        state_=State::prepared;out=descriptor_;return true;
    }
    bool accept(const NativeStartDescriptor &echo,uint64_t now) noexcept {
        if(state_!=State::prepared||!health(now))return fail();
        NativeStartDescriptor accepted;
        if(!start_.accept(echo,now,pending_.samples,accepted))return fail();
        pending_.epoch=accepted.clock.identity.epoch;pending_.firstFrame=0;pending_.frames=512;
        pendingSince_=now;state_=State::streaming;return true;
    }
    // Invoke on idle ticks too. One read and one finite-clock advance at most.
    // Unacknowledged batches remain owned here; repeated calls do not consume
    // additional source PCM. A stalled consumer has a caller-selected deadline.
    Result pump(uint64_t now) noexcept {
        if(state_==State::failed)return Result::failed;
        if(!health(now))return Result::failed;
        if(state_==State::ended)return Result::end;
        if(state_==State::empty||state_==State::prepared)return Result::waiting;
        SourceFrameClock::Stamp ignored;
        const auto clock=owner_.advance(now,ignored);
        using C=typename Owner::ClockResult;
        if(clock!=C::published&&clock!=C::waiting&&clock!=C::unavailable){fail();return Result::failed;}
        if(pending_.frames)return Result::batch;
        if(sealed_&&delivered_==end_){
            if(owner_.state()==Owner::State::finished){state_=State::ended;return Result::end;}
            return Result::waiting;
        }
        if(owner_.nextRead()!=delivered_){fail();return Result::failed;}
        const auto count=sealed_?std::min<uint64_t>(512,end_-delivered_):512;
        const auto read=owner_.read(std::span(pending_.samples).first(count),now);
        using R=typename Owner::ReadResult;
        if(read==R::waiting)return Result::waiting;
        if(read!=R::ok||owner_.nextRead()!=delivered_+count){fail();return Result::failed;}
        pending_.epoch=descriptor_.clock.identity.epoch;pending_.firstFrame=delivered_;
        pending_.frames=uint32_t(count);pendingSince_=now;return Result::batch;
    }
    // Copy only; acknowledge AFTER the consumer has accepted this exact range.
    // No borrowed span can outlive the producer. Failure leaves output unchanged.
    bool copy(uint64_t now,AudioFeedBatch &out) noexcept {
        if(!health(now)||state_!=State::streaming||!pending_.frames)return false;
        out=pending_;return true;
    }
    bool acknowledge(uint64_t epoch,uint64_t first,uint32_t frames,uint64_t now) noexcept {
        if(!health(now)||state_!=State::streaming||!pending_.frames||epoch!=pending_.epoch||
           first!=pending_.firstFrame||frames!=pending_.frames)return fail();
        delivered_+=frames;pending_={};return true;
    }
    bool finish(uint64_t end,uint64_t now,bool sourceAccepted) noexcept {
        if(!health(now)||state_!=State::streaming||sealed_||!sourceAccepted||
           end<owner_.nextRead()||end!=owner_.produced()||end%8||!owner_.finish(end,now))return fail();
        sealed_=true;end_=end;return true;
    }
    void cancel() noexcept {fail();}
private:
    bool health(uint64_t now) noexcept {
        if(state_==State::failed)return false;
        if(now<lastTime_||!owner_.poll(now))return fail();
        lastTime_=now;
        if(state_==State::prepared&&!start_.poll(now))return fail();
        if(pending_.frames&&(now<pendingSince_||now-pendingSince_>pendingAge_))return fail();
        return true;
    }
    bool fail() noexcept {
        if(state_!=State::failed){state_=State::failed;start_.cancel();pending_={};owner_.stop();}
        return false;
    }
    Owner &owner_;PreparedNativeStart<> start_;NativeStartDescriptor descriptor_{};
    AudioFeedBatch pending_{};uint64_t pendingAge_,pendingSince_=0,lastTime_=0,delivered_=0,end_=0;
    State state_=State::empty;bool sealed_=false;
};
}
