// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "HalSyntheticState.hpp"
#include <cstdlib>
#include <servers/bootstrap.h>
namespace Pro40Audio {
// Optional local driver adapter. Service send right must be configured before
// first start; no bootstrap lookup, service installation or implicit fallback.
class HalSyntheticDriverState {
    HalSyntheticState state_;
    std::mutex control_;
    mach_port_t service_=MACH_PORT_NULL;
    bool attempted_=false;
    bool drain() {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
        for(;;) {
            const auto result=state_.drainStopped();
            if(result!=SyntheticControlResult::busy)return result==SyntheticControlResult::ok;
            if(std::chrono::steady_clock::now()>=deadline)return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
public:
    static constexpr const char *serviceName="local.pro40.synthetic-lab";
    using Change=HalSilenceState::Change;
    HalSyntheticDriverState(uint32_t numer,uint32_t denom):state_(numer,denom) {}
    ~HalSyntheticDriverState() {
        // External callback/control quiescence is mandatory before destruction.
        state_.drainStopped();
        if(MACH_PORT_VALID(service_))mach_port_deallocate(mach_task_self(),service_);
    }
    bool configure(mach_port_t service) {
        std::lock_guard lock(control_);
        if(attempted_ || MACH_PORT_VALID(service_) || !MACH_PORT_VALID(service))return false;
        if(mach_port_mod_refs(mach_task_self(),service,MACH_PORT_RIGHT_SEND,1)!=KERN_SUCCESS)return false;
        service_=service;return true;
    }
    bool read(HalSilenceModel::Snapshot &out) const noexcept {return state_.snapshot(out);}
    bool change(Change action,uint32_t client,uint64_t,bool *runningChanged) {
        std::lock_guard lock(control_);if(runningChanged)*runningChanged=false;
        HalSilenceModel::Snapshot before;if(!state_.snapshot(before))return false;
        HalSyntheticChange result;
        if(action==Change::start && !before.running()) {
            attempted_=true;
            if(!before.known(client) || !before.numer || !before.denom ||
               !drain() || before.seed==UINT64_MAX)return false;
            // Control path only; bootstrap lookup itself has no timeout API.
            if(!MACH_PORT_VALID(service_) && bootstrap_look_up(bootstrap_port,serviceName,&service_)!=KERN_SUCCESS)return false;
            uint64_t epoch=0,nonce=0;
            do {arc4random_buf(&epoch,sizeof(epoch));}while(!epoch);
            do {arc4random_buf(&nonce,sizeof(nonce));}while(!nonce);
            const auto lead=uint64_t(__uint128_t(750000000)*before.denom/before.numer);
            const auto now=mach_absolute_time();if(lead>UINT64_MAX-now)return false;
            AudioSyntheticStart proposal{nonce,epoch,before.seed+1,now+lead,before.numer,before.denom,256000};
            result=state_.change(action,client,service_,&proposal);
        } else result=state_.change(action,client);
        if(result.accepted && action==Change::stop && result.runningChanged)drain();
        // A drain timeout retains closed/pinned storage. Next start retries
        // drain before preparation; never undo a committed stop or free early.
        if(runningChanged)*runningChanged=result.runningChanged;
        return result.accepted;
    }
    bool timestamp(uint32_t client,uint64_t now,HalSilenceModel::Stamp &out) noexcept {return state_.timestamp(client,now,out);}
    auto readInput(uint32_t client,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<float> out) noexcept {
        return state_.readInput(client,cycle,frames,out);
    }
};
}
