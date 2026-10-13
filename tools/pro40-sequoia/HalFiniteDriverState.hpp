// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "HalFiniteHostState.hpp"
#include <servers/bootstrap.h>
#include <os/log.h>
namespace Pro40Audio {
// Explicit finite laboratory service. Never fall back to the streaming service.
class HalFiniteDriverState {
    HalFiniteHostState state_;
    std::mutex control_;
    mach_port_t service_=MACH_PORT_NULL;
    bool attempted_=false;
    std::atomic<bool> cancel_{false};
public:
    using Change=HalSilenceState::Change;
    static constexpr const char *serviceName="local.pro40.finite-lab";
    HalFiniteDriverState(uint32_t numer,uint32_t denom):state_(numer,denom) {}
    ~HalFiniteDriverState() {
        // Host must stop/drain and quiesce all callbacks before Release.
        state_.drainStopped();
        if(MACH_PORT_VALID(service_))mach_port_deallocate(mach_task_self(),service_);
    }
    bool configure(mach_port_t service) {
        std::lock_guard lock(control_);
        if(attempted_ || MACH_PORT_VALID(service_) || !MACH_PORT_VALID(service))return false;
        if(mach_port_mod_refs(mach_task_self(),service,MACH_PORT_RIGHT_SEND,1)!=KERN_SUCCESS)return false;
        service_=service;return true;
    }
    bool read(HalSilenceModel::Snapshot &s) const noexcept{return state_.snapshot(s);}
    bool change(Change action,uint32_t client,uint64_t,bool *runningChanged) {
        std::lock_guard lock(control_);if(runningChanged)*runningChanged=false;
        if(action==Change::add)return state_.add(client);
        if(action==Change::remove)return state_.remove(client);
        HalFiniteHostState::Result result;
        if(action==Change::stop)result=state_.stop(client);
        else {
            HalSilenceModel::Snapshot s;if(!state_.snapshot(s))return false;
            AudioSyntheticStart proposal;
            if(!s.running()) {
                attempted_=true;
                if(!s.known(client) || !s.numer || !s.denom || s.seed==UINT64_MAX)return false;
                // Control-only lookup has no timeout API. Preparation after it
                // has the host adapter's admission/readiness bounds.
                if(!MACH_PORT_VALID(service_) && bootstrap_look_up(bootstrap_port,serviceName,&service_)!=KERN_SUCCESS)return false;
                uint64_t nonce=0,epoch=0;
                do {arc4random_buf(&nonce,sizeof(nonce));}while(!nonce);
                do {arc4random_buf(&epoch,sizeof(epoch));}while(!epoch);
                const auto now=mach_absolute_time();
                const auto lead=uint64_t(__uint128_t(700000000)*s.denom/s.numer);
                if(lead>UINT64_MAX-now)return false;
                proposal={nonce,epoch,s.seed+1,now+lead,s.numer,s.denom,256000};
            }
            result=state_.start(client,service_,proposal,cancel_);
        }
        if(runningChanged)*runningChanged=result.runningChanged;
        if(!result.accepted || !result.cleanupOK)
            os_log_error(OS_LOG_DEFAULT,"Pro40Lab finite control failure: action=%{public}u error=%{public}u cleanup=%{public}u admission=%{public}d",
                unsigned(action),unsigned(result.error),unsigned(result.cleanupOK),int(result.admissionStatus));
        // Accepted stop stays committed even if cleanup needs another drain.
        return result.accepted;
    }
    bool timestamp(uint32_t client,uint64_t now,HalSilenceModel::Stamp &out) noexcept{return state_.timestamp(client,now,out);}
    auto readInput(uint32_t client,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<float> out) noexcept {
        return state_.readInput(client,cycle,frames,out);
    }
};
}
