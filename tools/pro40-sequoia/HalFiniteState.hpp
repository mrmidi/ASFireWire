// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFiniteCallbackSession.hpp"
#include "AudioFeedWorker.hpp"
#include "AudioSessionGate.hpp"
#include "HalSilenceState.hpp"
#include <pthread.h>
namespace Pro40Audio {
// Local HAL lifecycle lab with injected transport. Explicit completion policy;
// no native startup, host notifications, service admission or driver install.
template<class Transport> class HalFiniteState {
public:
    // Single-reader ACK is for diagnostic callers with one logical obligation.
    // Shared HAL clients cannot prove collective drain from one callback.
    enum class Completion {singleReader,sharedUntilStopOrTimeout};
    enum class StartResult {started,waiting,invalid,late,sourceFinished,rejected};
    struct Clock {uint64_t epoch=0,seed=0,anchor=0;uint32_t numer=0,denom=0;};
    class Session {
        using Worker=AudioFeedWorker<8192,Transport,AudioFiniteCallbackSession<8192>>;
        mutable Worker worker_;
        pthread_t thread_{};
        bool launched_=false;
        std::atomic<bool> entered_{false};
        static void *entry(void *p) {
            auto &s=*static_cast<Session *>(p);s.entered_.store(true,std::memory_order_release);s.worker_.run();return nullptr;
        }
    public:
        const Clock clock;
        const uint32_t client;
        const Completion completion;
        Session(Transport transport,Clock c,uint32_t id,Completion policy=Completion::singleReader):
            worker_(std::move(transport),AudioCallbackTimeline{c.epoch,c.seed,0,0},0),clock(c),client(id),completion(policy) {}
        ~Session(){stop();}
        bool launch() {
            if(launched_ || worker_.report())return false;
            launched_=pthread_create(&thread_,nullptr,entry,this)==0;return launched_;
        }
        bool ready() const {return launched_ && entered_.load(std::memory_order_acquire) && !worker_.report();}
        bool hasInitialAudio(UInt32 frames) const noexcept {
            if(!frames || frames>AudioCallbackTimeline::maxFrames)return false;
            AudioServerPlugInIOCycleInfo cycle{};
            cycle.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
            std::array<float,AudioCallbackTimeline::maxFrames> samples;
            const auto result=read(cycle,frames,std::span(samples).first(frames));
            return result.ok() && result.audioFrames==frames && !result.paddingFrames;
        }
        bool stop() {
            worker_.requestStop();
            if(launched_){pthread_join(thread_,nullptr);launched_=false;}
            else if(!worker_.report())worker_.run();
            auto r=worker_.report();return r && r->transportClosed;
        }
        auto report() const noexcept {return worker_.report();}
        auto read(const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<float> out) const noexcept {
            return worker_.readInput(clock.epoch,clock.seed,cycle,frames,out);
        }
        bool complete(const FiniteCallbackResult &r) const noexcept {
            return completion==Completion::singleReader && worker_.completeRead(r);
        }
    };
private:
    HalSilenceState state_;
    std::mutex control_;
    AudioSessionGate<Session> gate_;
    bool stopping_=false,blocked_=false;
    uint64_t previousEpoch_=0;
    using Lease=typename AudioSessionGate<Session>::Lease;
    std::optional<Lease> active(uint32_t client,HalSilenceModel::Snapshot &s) noexcept {
        auto pin=gate_.tryAcquire();if(!pin || !state_.read(s) || !s.active(client))return {};
        const auto &session=pin->get();const auto &c=session.clock;
        if((session.completion==Completion::singleReader && session.client!=client) ||
           c.seed!=s.seed || c.anchor!=s.anchor || c.numer!=s.numer || c.denom!=s.denom)return {};
        return pin;
    }
    bool drain() {
        if(!stopping_)return !blocked_;
        auto old=gate_.tryReclaim();if(!old)return false;
        if(!old->stop())blocked_=true;
        stopping_=false;return !blocked_;
    }
public:
    HalFiniteState(uint32_t numer,uint32_t denom):state_(numer,denom) {}
    bool add(uint32_t client) {std::lock_guard lock(control_);return state_.change(HalSilenceState::Change::add,client);}
    bool remove(uint32_t client) {
        std::lock_guard lock(control_);
        auto pin=gate_.tryAcquire();
        // Preserve the single-reader diagnostic's only stop identity.
        if(pin && pin->get().completion==Completion::singleReader && pin->get().client==client)return false;
        return state_.change(HalSilenceState::Change::remove,client);
    }
    bool retainIO(uint32_t client) {
        std::lock_guard lock(control_);HalSilenceModel::Snapshot s;
        auto pin=gate_.tryAcquire();
        if(blocked_ || stopping_ || !pin || pin->get().completion!=Completion::sharedUntilStopOrTimeout ||
           !state_.read(s) || !s.running())return false;
        return state_.change(HalSilenceState::Change::start,client);
    }
    bool snapshot(HalSilenceModel::Snapshot &s) const noexcept {return state_.read(s);}
    // Control-owner polling only: no waits, automatic retries, clock estimation
    // or source pacing. now/maxLate use the same ticks as Clock::anchor.
    // Caller retains rejected/late sessions and must stop/join them. The
    // underlying start() remains available for isolated lifecycle fixtures.
    StartResult startReady(std::unique_ptr<Session> &prepared,uint64_t now,
                           UInt32 initialFrames,uint64_t maxLate) {
        if(!prepared || !initialFrames || initialFrames>AudioCallbackTimeline::maxFrames ||
           !maxLate || !prepared->clock.anchor)return StartResult::invalid;
        if(prepared->report())return StartResult::sourceFinished;
        if(now<prepared->clock.anchor)return StartResult::waiting;
        if(now-prepared->clock.anchor>maxLate)return StartResult::late;
        if(!prepared->ready() || !prepared->hasInitialAudio(initialFrames))return StartResult::waiting;
        return start(prepared)?StartResult::started:StartResult::rejected;
    }
    // Prepared session is caller-owned until successful gate publication.
    bool start(std::unique_ptr<Session> &prepared) {
        std::lock_guard lock(control_);HalSilenceModel::Snapshot before;
        if(blocked_ || stopping_ || !prepared || !prepared->ready() || !state_.read(before) || before.running())return false;
        const auto &c=prepared->clock;const auto client=prepared->client;
        if(!before.known(client) || !c.epoch || c.epoch==previousEpoch_ || !c.anchor ||
           before.seed==UINT64_MAX || c.seed!=before.seed+1 || c.numer!=before.numer || c.denom!=before.denom)return false;
        if(!state_.change(HalSilenceState::Change::start,client,c.anchor))return false;
        // Briefly running without a published session yields failed/zero reads.
        if(!gate_.publish(prepared)) {
            state_.change(HalSilenceState::Change::stop,client);blocked_=true;return false;
        }
        previousEpoch_=c.epoch;return true;
    }
    bool stop(uint32_t client) {
        std::lock_guard lock(control_);
        {auto pin=gate_.tryAcquire();if(!pin ||
            (pin->get().completion==Completion::singleReader && pin->get().client!=client))return false;}
        bool ended=false;
        if(!state_.change(HalSilenceState::Change::stop,client,0,&ended))return false;
        if(!ended)return true;
        gate_.closeAdmission();stopping_=true;return drain();
    }
    bool drainStopped() {std::lock_guard lock(control_);return drain();}
    bool timestamp(uint32_t client,uint64_t now,HalSilenceModel::Stamp &stamp) noexcept {
        HalSilenceModel::Snapshot s;auto pin=active(client,s);
        return pin && HalSilenceModel::timestamp(s,now,stamp);
    }
    FiniteCallbackResult readInput(uint32_t client,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<float> out) noexcept {
        HalSilenceModel::Snapshot s;auto pin=active(client,s);
        if(!pin){std::fill(out.begin(),out.end(),0);return {};}
        return pin->get().read(cycle,frames,out);
    }
    bool completeRead(uint32_t client,const FiniteCallbackResult &r) noexcept {
        HalSilenceModel::Snapshot s;auto pin=active(client,s);return pin && pin->get().complete(r);
    }
    auto report(uint32_t client) noexcept {
        HalSilenceModel::Snapshot s;auto pin=active(client,s);
        return pin?pin->get().report():std::optional<WorkerReport>{};
    }
    // Stop, drain and externally quiesce ALL callback/control callers before
    // destroying this state/gate. EOF alone does not stop the HAL clock/IO hold.
};
}
