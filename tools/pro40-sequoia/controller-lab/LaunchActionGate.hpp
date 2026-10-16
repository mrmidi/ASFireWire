// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RegistrationJournal.hpp"
namespace Pro40Lab {
enum class ActionPermission { allowed, cancelled, expired, invalidObservation };
// One serial owner retains the journal through operations and reconciliation.
// This gate does not own resources or infer cleanup. Permit must freshly check
// cancellation/deadline and ownership; action must issue immediately, not enqueue.
class LaunchActionGate final {
    RegistrationJournal& journal_;
    bool blocked_ = false;
    bool launchUsed_ = false, releaseUsed_ = false;
    bool unregisterUsed_ = false;
    void reject(ActionPermission p, NSString *detail, uint64_t now) {
        blocked_=true;
        NSString *category=p==ActionPermission::cancelled ? @"cancelled" :
            p==ActionPermission::expired ? @"expired" : @"observation-invalid";
        (void)journal_.recordLaunchFailure(category,detail,now);
    }
    template<class Permit, class Action, class Now>
    bool perform(NSString *kind, NSDictionary *evidence, bool& used,
                 Permit&& permit, Action&& action, Now&& now) {
        if (blocked_ || used) return false;
        used = true;
        auto permission=permit();
        if (permission!=ActionPermission::allowed) {
            reject(permission,@"action permission denied before intent",now()); return false;
        }
        if (!journal_.append(kind,evidence)) { blocked_=true; return false; }
        // Persistence can run past a deadline. Keep the intent, never act on stale permission.
        permission=permit();
        if (permission!=ActionPermission::allowed) {
            reject(permission,@"action permission denied after intent",now()); return false;
        }
        action();
        return true; // issued only; not proof of success, exit or cleanup
    }
public:
    explicit LaunchActionGate(RegistrationJournal& journal) : journal_(journal) {}
    LaunchActionGate(const LaunchActionGate&) = delete;
    LaunchActionGate& operator=(const LaunchActionGate&) = delete;
    template<class Permit,class Action,class Now>
    bool launch(NSDictionary *e, Permit&& p, Action&& a, Now&& n) {
        return perform(@"launch_intent",e,launchUsed_,p,a,n);
    }
    template<class Permit,class Action,class Now>
    bool release(NSDictionary *e, Permit&& p, Action&& a, Now&& n) {
        if (!launchUsed_) return false;
        return perform(@"release_intent",e,releaseUsed_,p,a,n);
    }
    bool fail(NSString *category, NSString *detail, uint64_t observedNS) {
        if(blocked_)return false;
        blocked_=true;
        return journal_.recordLaunchFailure(category,detail,observedNS);
    }
    template<class Permit,class Action,class Now>
    bool unregister(Permit&& p, Action&& a, Now&& n) {
        if(!releaseUsed_)return false;
        return perform(@"unregister_intent",@{},unregisterUsed_,p,a,n);
    }
};
}
