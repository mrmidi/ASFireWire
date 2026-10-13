// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeServiceEntryGate.hpp"
namespace Pro40Audio {
// Evidence only. This object never releases controller resources, cleans a device
// lease or clears a persistent journal. Owner keeps unresolved resources alive.
// Controller evidence covers resources OUTSIDE the verifier and runner (e.g. the
// original sender). Unknown is the default; runner cleanup cannot substitute for it.
struct ServiceControllerCleanup {
    LocalCleanup local=LocalCleanup::incomplete;
    RemoteCleanup remote=RemoteCleanup::unconfirmed;
};
class NativeServiceAttemptResult {
    const ServiceEntryResult entry_;
    const int32_t serviceCleanup_,controllerCleanup_;
    const bool valid_;
public:
    NativeServiceAttemptResult(const NativeServiceIdentityDecision &decision,ServiceEntryResult entry,
        ServiceControllerCleanup controller={})noexcept:
        entry_(std::move(entry)),
        serviceCleanup_(peerCleanupStatus(decision.combineLocalCleanup(LocalCleanup::closed),
                                         decision.combineRemoteCleanup(RemoteCleanup::notRequired))),
        controllerCleanup_(peerCleanupStatus(controller.local,controller.remote)),
        valid_((entry_.status==ServiceEntryStatus::entered&&entry_.session&&decision.permitsAdmission())||
               (entry_.status==ServiceEntryStatus::denied&&!entry_.session&&!decision.permitsAdmission())||
               (entry_.status==ServiceEntryStatus::stopped&&!entry_.session&&decision.permitsAdmission())){}
    const ServiceEntryResult &entry()const noexcept{return entry_;}
    int32_t serviceCleanup()const noexcept{return serviceCleanup_;}
    int32_t controllerCleanup()const noexcept{return controllerCleanup_;}
    bool valid()const noexcept{return valid_;}
    bool cleanupComplete()const noexcept{
        if(!valid_||serviceCleanup_||controllerCleanup_)return false;
        if(entry_.status!=ServiceEntryStatus::entered)return true;
        // No outcome is uncertainty here, even if an exit code suggests an early
        // failure. The controller must resolve that through its ownership contract.
        return entry_.session->outcome&&entry_.session->outcome->cleanup==0;
    }
    bool accepted()const noexcept{
        return cleanupComplete()&&entry_.status==ServiceEntryStatus::entered&&entry_.session->accepted();
    }
};
}
