// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "HalFiniteState.hpp"
#include "AudioFeedMachTransport.hpp"
#include "AudioSyntheticAdmission.hpp"
namespace Pro40Audio {
// Local host-facing control adapter, not selected by the installed driver.
// Callbacks never take this control mutex. Notify host AFTER methods return.
class HalFiniteHostState {
    using State=HalFiniteState<AudioFeedMachTransport>;
    State state_;
    std::mutex control_;
    bool blocked_=false;
    bool drain() {
        const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
        do {
            if(state_.drainStopped())return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while(std::chrono::steady_clock::now()<until);
        return false;
    }
public:
    enum class Error {none,invalid,cancelled,admission,thread,startup,cleanup};
    struct Result {
        bool accepted=false,runningChanged=false,cleanupOK=true;
        Error error=Error::none;
        kern_return_t admissionStatus=KERN_SUCCESS;
    };
    HalFiniteHostState(uint32_t numer,uint32_t denom):state_(numer,denom) {}
    bool add(uint32_t client){std::lock_guard lock(control_);return state_.add(client);}
    bool remove(uint32_t client){std::lock_guard lock(control_);return state_.remove(client);}
    bool snapshot(HalSilenceModel::Snapshot &s) const noexcept{return state_.snapshot(s);}
    // service is borrowed for this synchronous call; it must implement finiteEnd.
    // cancel storage must outlive call. Cancellation cannot interrupt an in-flight
    // admission Mach call (each bounded to 500ms). No bootstrap lookup here.
    Result start(uint32_t client,mach_port_t service,const AudioSyntheticStart &proposal,
                 const std::atomic<bool> &cancel) {
        std::lock_guard lock(control_);HalSilenceModel::Snapshot before;
        if(blocked_)return {false,false,false,Error::cleanup};
        if(!state_.snapshot(before) || !before.known(client))return {false,false,true,Error::invalid};
        if(cancel.load())return {false,false,true,Error::cancelled};
        if(before.running())return {state_.retainIO(client),false,true,Error::none};
        if(!drain())return {false,false,false,Error::cleanup};
        const auto now=mach_absolute_time();
        if(!MACH_PORT_VALID(service) || !SyntheticAdmission::ready(proposal) || proposal.origin<=now ||
           __uint128_t(proposal.origin-now)*proposal.numer>__uint128_t(750000000)*proposal.denom ||
           proposal.numer!=before.numer || proposal.denom!=before.denom || before.seed==UINT64_MAX ||
           proposal.seed!=before.seed+1)return {false,false,true,Error::invalid};
        mach_port_t receive=MACH_PORT_NULL;
        if(mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&receive)!=KERN_SUCCESS)
            return {false,false,true,Error::admission};
        auto prepared=std::make_unique<State::Session>(AudioFeedMachTransport(receive),
            State::Clock{proposal.epoch,proposal.seed,proposal.origin,proposal.numer,proposal.denom},client,
            State::Completion::sharedUntilStopOrTimeout);
        auto rollback=[&](Error why,kern_return_t admissionStatus=KERN_SUCCESS) {
            const bool clean=prepared->stop();if(!clean)blocked_=true;
            return Result{false,false,clean,clean?why:Error::cleanup,admissionStatus};
        };
        const auto admission=SyntheticAdmission::subscribe(service,receive,proposal);
        if(admission!=KERN_SUCCESS)return rollback(Error::admission,admission);
        if(cancel.load())return rollback(Error::cancelled);
        if(!prepared->launch())return rollback(Error::thread);
        const auto late=uint64_t((__uint128_t(100000000)*proposal.denom+proposal.numer-1)/proposal.numer);
        for(;;) {
            if(cancel.load())return rollback(Error::cancelled);
            const auto result=state_.startReady(prepared,mach_absolute_time(),512,late);
            if(result==State::StartResult::started)return {true,true,true,Error::none};
            if(result!=State::StartResult::waiting)return rollback(Error::startup);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    Result stop(uint32_t client) {
        std::lock_guard lock(control_);HalSilenceModel::Snapshot before,after;
        if(!state_.snapshot(before) || !before.active(client))return {false,false,true,Error::invalid};
        const bool immediate=state_.stop(client);
        if(!state_.snapshot(after))return {false,false,false,Error::cleanup};
        const bool accepted=after.ioHolds+1==before.ioHolds;
        const bool ended=accepted && !after.running();
        const bool clean=ended?drain():immediate;
        // A committed stop is never rolled back because reclamation is delayed.
        return {accepted,ended,clean,clean?Error::none:Error::cleanup};
    }
    bool drainStopped(){std::lock_guard lock(control_);return drain();}
    bool timestamp(uint32_t client,uint64_t now,HalSilenceModel::Stamp &out) noexcept{return state_.timestamp(client,now,out);}
    auto readInput(uint32_t client,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<float> out) noexcept {
        return state_.readInput(client,cycle,frames,out);
    }
    auto report(uint32_t client) noexcept{return state_.report(client);}
    // Caller must stop all holds, finish drain, and quiesce ALL callers before
    // destruction. Failed preparation is owned and joined here; active IO is not.
};
}
