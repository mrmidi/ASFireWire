// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "JournaledVerifierLaunch.hpp"
#include "NativeServiceOperations.hpp"
#include <atomic>
#include <functional>
namespace Pro40Lab {
enum class VerifierCleanupState { initial, awaitingCallback, awaitingLocalCleanup, complete, blocked };
// Resources owns journal, launch (JournaledVerifierLaunch), and the SAME operations
// object that registered the service. Supervisor retains this owner until
// readyToRetire. Missing OS callbacks retain ownership; timeouts cannot cancel them.
template<class Resources>
class VerifierCleanupOwner final : public std::enable_shared_from_this<VerifierCleanupOwner<Resources>> {
    dispatch_queue_t queue_;
    std::shared_ptr<Resources> resources_;
    std::function<NSDictionary *()> observe_;
    std::function<bool()> localDone_;
    std::function<uint64_t()> now_;
    uint64_t deadline_,last_=0;
    std::atomic<bool> stop_{false};
    bool pending_=false;
    VerifierCleanupState state_=VerifierCleanupState::initial;
    ActionPermission permission(){
        const auto t=now_();
        if(stop_.load())return ActionPermission::cancelled;
        if(t<last_)return ActionPermission::invalidObservation;
        last_=t;return t>=deadline_?ActionPermission::expired:ActionPermission::allowed;
    }
    void block(){
        if(state_==VerifierCleanupState::complete || state_==VerifierCleanupState::blocked)return;
        state_=VerifierCleanupState::blocked;
        const auto t=now_();
        resources_->launch->abandon(stop_.load()?@"cancelled":t>=deadline_?@"expired":@"observation-invalid",@"cleanup owner blocked",t);
    }
    void finish(){
        if(state_!=VerifierCleanupState::awaitingLocalCleanup)return;
        if(permission()!=ActionPermission::allowed){block();return;}
        if(!localDone_())return;
        if(permission()!=ActionPermission::allowed){block();return;}
        if(!resources_->launch->complete(true,now_())){block();return;}
        state_=VerifierCleanupState::complete;
    }
    void returned(NSDictionary *result){
        dispatch_assert_queue(queue_);pending_=false;
        if(state_!=VerifierCleanupState::awaitingCallback)return; // late completion never reopens a blocked record
        if(permission()!=ActionPermission::allowed || !resources_->launch->unregisterReturned(result)){block();return;}
        if(permission()!=ActionPermission::allowed){block();return;}
        NSDictionary *removal=observe_();
        if(permission()!=ActionPermission::allowed || !resources_->launch->removed(removal,now_())){block();return;}
        state_=VerifierCleanupState::awaitingLocalCleanup;finish();
    }
    VerifierCleanupOwner(dispatch_queue_t q,std::shared_ptr<Resources> r,std::function<NSDictionary *()> o,
        std::function<bool()> l,std::function<uint64_t()> n,uint64_t d):queue_(q),resources_(std::move(r)),
        observe_(std::move(o)),localDone_(std::move(l)),now_(std::move(n)),deadline_(d){}
public:
    static std::shared_ptr<VerifierCleanupOwner> create(dispatch_queue_t q,std::shared_ptr<Resources> r,
        std::function<NSDictionary *()> observe,std::function<bool()> localDone,std::function<uint64_t()> now,uint64_t deadline){
        if(!q || !r || !r->launch || !observe || !localDone || !now || !deadline)return {};
        return std::shared_ptr<VerifierCleanupOwner>(new VerifierCleanupOwner(q,std::move(r),std::move(observe),std::move(localDone),std::move(now),deadline));
    }
    void requestStop(){stop_.store(true);}
    void start(){
        dispatch_assert_queue(queue_);if(state_!=VerifierCleanupState::initial)return;
        state_=VerifierCleanupState::awaitingCallback;
        bool issued=resources_->launch->unregister([&]{return permission();},[&]{
            pending_=true;auto retained=this->shared_from_this();
            bool handed=resources_->operations.unregisterOnce([]{return true;},^(NSDictionary *r){retained->returned(r);});
            if(!handed){pending_=false;block();}
        },now_);
        if(!issued)block();
    }
    void poll(){
        dispatch_assert_queue(queue_);
        if(state_==VerifierCleanupState::awaitingCallback && permission()!=ActionPermission::allowed)block();
        finish();
    }
    VerifierCleanupState state()const{dispatch_assert_queue(queue_);return state_;}
    bool callbackPending()const{dispatch_assert_queue(queue_);return pending_;}
    bool localCleanupComplete()const{dispatch_assert_queue(queue_);return localDone_();}
    bool cleanExitAndRemoval()const{dispatch_assert_queue(queue_);return resources_->launch->cleanExitAndRemoval();}
    bool readyToRetire()const{
        dispatch_assert_queue(queue_);
        return !pending_ && (state_==VerifierCleanupState::complete || state_==VerifierCleanupState::blocked) && localDone_();
    }
};
}
