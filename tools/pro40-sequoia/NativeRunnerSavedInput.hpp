// SPDX-License-Identifier: Apache-2.0
#pragma once
// Test-only saved content driver, not a hardware emulator. Source callbacks run
// inside the runner fixture's preparation hook; wall waits are outside callbacks.
#include "NativeProducerConsumer.hpp"
#include "NativeStartupConsumer.hpp"
#include "NativePeerSession.hpp"
#include "NativeConsumerBridge.hpp"
#include "NativeSourceCompletion.hpp"
#include <cassert>
#include <iostream>
#include <memory>
#include <vector>
struct RunnerSavedClock {
    static inline uint64_t value=0,step=0,reads=0;
    static uint64_t now(){const auto result=value;value+=step;++reads;return result;}
};
template<class Resources> class NativeRunnerSavedInput {
    using Consumer=Pro40Audio::NativeProducerConsumer<RunnerSavedClock>;
    using Bridge=Pro40Audio::NativeConsumerBridge<Consumer,RunnerSavedClock,Resources>;
    using Wall=std::chrono::steady_clock;
    Pro40Audio::NativeFeedMailbox &feed_;
    Pro40Audio::SourceFrameClock::Identity id_{};
    std::vector<int32_t> pcm_;
    Pro40Packets::InputRecording recording_;
    std::unique_ptr<Consumer> consumer_;
    std::unique_ptr<Pro40Audio::NativeSourceCompletion<Consumer>> completion_;
    std::unique_ptr<Bridge> bridge_;
    std::unique_ptr<Pro40::NativeTimingEvidence> timing_;
    uint64_t checked_=0;bool senderEnded_=false; // Worker-owned until join.
    int delayMs_=-1;bool holdEnd_=false;int32_t completionStatus_=0;
public:
    NativeRunnerSavedInput(Pro40Audio::NativeFeedMailbox &feed,Pro40Audio::NativeStartMailbox &start,
                          Pro40Audio::NativePeerSession &peer,Resources &resources,int delayMs=-1,bool holdEnd=false):feed_(feed),delayMs_(delayMs),holdEnd_(holdEnd){
        uint64_t begin,age;size_t frames;
        assert(std::cin>>id_.epoch>>id_.seed>>id_.generation>>id_.numer>>id_.denom>>begin>>age>>frames);
        assert(frames&&frames<=2097152&&age);pcm_.resize(frames);
        for(auto &v:pcm_)assert(std::cin>>v);
        RunnerSavedClock::value=begin;RunnerSavedClock::step=0;RunnerSavedClock::reads=0;
        consumer_=std::make_unique<Consumer>(recording_,feed_,id_);
        completion_=std::make_unique<Pro40Audio::NativeSourceCompletion<Consumer>>(*consumer_,std::chrono::seconds(5));
        bridge_=std::make_unique<Bridge>(*consumer_,peer,start,id_,11,resources,std::chrono::seconds(5));
        timing_=std::make_unique<Pro40::NativeTimingEvidence>();
        timing_->numer=id_.numer;timing_->denom=id_.denom;
        assert(timing_->attachConsumer(bridge_->callbacks()));
    }
    uint64_t epoch()const{return id_.epoch;}
    // Sender interface: compares every frame; no real Mach audio transmission.
    bool send(const Pro40Audio::AudioFeedBatch &b){
        assert(b.epoch==id_.epoch&&b.firstFrame==checked_&&checked_<=pcm_.size()&&b.frames<=pcm_.size()-checked_);
        assert(std::equal(b.samples.begin(),b.samples.begin()+b.frames,pcm_.begin()+checked_));
        checked_+=b.frames;return true;
    }
    bool finish(uint64_t epoch,uint64_t end){
        assert(epoch==id_.epoch&&end==checked_&&end==pcm_.size());
        if(holdEnd_){
            const auto deadline=Wall::now()+std::chrono::seconds(5);
            while(!feed_.cancelled()){
                assert(Wall::now()<deadline);std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            return false; // Source's advancing-clock END deadline revoked receipt.
        }
        senderEnded_=true;return true;
    }
    void sender(){
        const auto deadline=Wall::now()+std::chrono::seconds(60);
        while(!senderEnded_&&!feed_.cancelled()){
            assert(Wall::now()<deadline);
            if(!Pro40Audio::sendNativeFeedOnce(feed_,*this)){assert(feed_.cancelled());return;}
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    auto preparation(){return bridge_->preparation();}
    auto finalization(){return bridge_->finalization();}
    void verifyAfterJoin(bool cancelled){
        if(delayMs_==-3){
            assert(completionStatus_!=0&&consumer_->failed()&&!senderEnded_);
            std::printf("SAVED: rejected %llu frames checked\n",(unsigned long long)checked_);return;
        }
        if(delayMs_>=0){
            if(delayMs_>=1100||holdEnd_)assert(completionStatus_!=0&&consumer_->failed()&&!senderEnded_);
            else assert(completionStatus_==0&&consumer_->ended()&&senderEnded_&&checked_==pcm_.size());
            std::printf("SAVED: %s %llu frames checked\n",completionStatus_?"rejected":"completed",(unsigned long long)checked_);
            return;
        }
        if(cancelled)assert(consumer_->failed()&&!senderEnded_&&checked_>=8192&&checked_<pcm_.size());
        else assert(consumer_->ended()&&senderEnded_&&checked_==pcm_.size()&&consumer_->delivered()==checked_);
        std::printf("SAVED: %s %llu frames checked\n",cancelled?"cancelled":"completed",(unsigned long long)checked_);
    }
    void drive(bool cancelEarly){
        char op;
        auto drain=[&]{
            const auto deadline=Wall::now()+std::chrono::seconds(5);
            do {
                assert(Wall::now()<deadline&&timing_->pollConsumer());
                if(consumer_->delivered()==consumer_->nextRead())return;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }while(true);
        };
        while(std::cin>>op){
            if(op=='P'){
                Pro40Audio::PacketClockFit::Packet p;
                assert(std::cin>>p.ordinal>>p.firstFrame>>p.generation>>p.arrivalCycle>>p.syt>>p.blocks>>p.dbc);
                assert(p.firstFrame<=pcm_.size()&&p.blocks<=pcm_.size()-p.firstFrame&&(p.blocks==0||p.blocks==8));
                // Same synthetic envelopes as NativeProducerConsumerReplay:
                // saved input-1 PCM plus saved timing, not original wire bytes.
                std::array<uint8_t,360> bytes{};
                Pro40Packets::write32(bytes.data(),(2u<<24)|(11u<<16)|p.dbc);
                Pro40Packets::write32(bytes.data()+4,0x90020000u|p.syt);
                for(unsigned n=0;n<p.blocks;++n){
                    for(unsigned c=0;c<10;++c)bytes[8+n*44+c*4]=0x40;
                    bytes[8+n*44+40]=0x80;const auto bits=uint32_t(pcm_[p.firstFrame+n]);
                    bytes[9+n*44]=uint8_t(bits>>16);bytes[10+n*44]=uint8_t(bits>>8);bytes[11+n*44]=uint8_t(bits);
                }
                const auto payload=std::span(bytes).first(p.blocks?360:8);
                recording_.observe(payload);timing_->packets.observe(payload,p.arrivalCycle,p.generation,2);
                assert(!timing_->packets.fault()&&!timing_->consumerFailed());
            }else if(op=='C'){
                Pro40::NativeTimingEvidence::Clock p;unsigned anchor;
                assert(std::cin>>p.before>>p.uptime>>p.after>>p.generation>>p.cycle>>p.status>>p.packetCount>>anchor);
                assert(anchor<=1);p.beforeReceive=anchor;RunnerSavedClock::value=p.after;timing_->observe(p);
                assert(!timing_->fault&&!timing_->consumerFailed());
                if(bridge_->state()!=decltype(bridge_->state())::empty){
                    const auto deadline=Wall::now()+std::chrono::seconds(5);
                    while(bridge_->state()!=decltype(bridge_->state())::active){
                        assert(Wall::now()<deadline&&timing_->pollConsumer());
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                    drain();
                    if(cancelEarly&&consumer_->delivered()>=8192){bridge_->cancelSource();return;}
                }
            }else if(op=='E'){
                assert(!cancelEarly);
                if(delayMs_<=-2){
                    uint64_t measured;assert(std::cin>>measured);
                    assert(measured>=RunnerSavedClock::value);
                    const auto selected=consumer_->selectedClockEvidence();assert(selected);
                    assert(selected->pairs&&selected->pairs<=timing_->used);
                    assert(selected->observedHost==timing_->clocks[selected->pairs-1].after);
                    const auto latest=timing_->clocks[timing_->used-1].after;
                    if(delayMs_==-3){
                        const auto useAge=uint64_t(__uint128_t(1000000000)*id_.denom/id_.numer);
                        measured=selected->observedHost+useAge+1;
                        assert(measured>RunnerSavedClock::value&&measured-latest<useAge);
                    }
                    assert(selected->observedHost<=latest&&latest<=measured);
                    auto ns=[&](uint64_t ticks){return (unsigned long long)(__uint128_t(ticks)*id_.numer/id_.denom);};
                    std::printf("MODEL: selected-pairs=%llu total-pairs=%zu observed=%llu completion=%llu selected-age-ns=%llu latest-age-ns=%llu\n",
                        (unsigned long long)selected->pairs,timing_->used,(unsigned long long)selected->observedHost,
                        (unsigned long long)measured,ns(measured-selected->observedHost),ns(measured-latest));
                    RunnerSavedClock::value=measured;
                    RunnerSavedClock::step=uint64_t(__uint128_t(100000)*id_.denom/id_.numer);
                    assert(RunnerSavedClock::step);
                }
                if(delayMs_>=0){
                    RunnerSavedClock::value+=uint64_t(__uint128_t(delayMs_)*1000000*id_.denom/id_.numer);
                    RunnerSavedClock::step=uint64_t(__uint128_t(100000)*id_.denom/id_.numer);
                    assert(RunnerSavedClock::step);
                }
                const auto before=RunnerSavedClock::reads;
                completionStatus_=completion_->hook().status(true,{});
                if(delayMs_==-3)assert(completionStatus_!=0&&consumer_->failed());
                else if(delayMs_<0)assert(completionStatus_==0&&consumer_->ended());
                else std::printf("DRAIN: delay-ms=%d hold-end=%u step-ns=100000 clock-reads=%llu result=%d\n",delayMs_,unsigned(holdEnd_),
                    (unsigned long long)(RunnerSavedClock::reads-before),completionStatus_);
                assert(completion_->hook().status(true,{})==completionStatus_);return;
            }else assert(false);
        }
        assert(false);
    }
};
