// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeIdentityDecision.hpp"
#include <IOKit/IOReturn.h>
#include <type_traits>
namespace Pro40Audio {
// Serialized resource policy for NativeConsumerBridge's existing prepare/finalize
// hooks. Not a provisioner or another lifecycle. Verify supplies trusted, bounded,
// nonthrowing outcome acquisition; the synchronous IdentityLab verifier is NOT
// suitable for live use until its blocking calls/trust assumptions are resolved.
// All collaborators and any unresolved outcome must outlive failed finalization.
template<class Resources,class Verify> class NativeIdentityBridgeResources {
    static_assert(std::is_nothrow_invocable_r_v<IdentityLabOutcome,Verify &>,
                  "verification must return a checked outcome without throwing");
    Resources &resources_;
    Verify &verify_;
    std::optional<NativeIdentityDecision> decision_;
    bool attempted_=false,touched_=false;
public:
    NativeIdentityBridgeResources(Resources &resources,Verify &verify):resources_(resources),verify_(verify){}
    NativeIdentityBridgeResources(const NativeIdentityBridgeResources &)=delete;
    NativeIdentityBridgeResources &operator=(const NativeIdentityBridgeResources &)=delete;
    int32_t prepare(){
        if(attempted_)return kIOReturnNotPermitted;
        attempted_=true;
        decision_.emplace(verify_());
        if(!decision_->permitsAdmission())return kIOReturnNotPermitted;
        touched_=true; // Partial downstream preparation still requires cleanup.
        return resources_.prepare();
    }
    void cancel(){if(touched_)resources_.cancel();}
    bool confirmRemote(){return !touched_ || resources_.confirmRemote();}
    bool closeControl(){
        // Always attempt downstream cleanup, even if helper cleanup is blocked.
        const auto other=(!touched_ || resources_.closeControl())?LocalCleanup::closed:LocalCleanup::incomplete;
        return decision_ && decision_->combineLocalCleanup(other)==LocalCleanup::closed;
    }
    const NativeIdentityDecision *decision()const noexcept{return decision_?&*decision_:nullptr;}
    // Explicit supervisor observation; never clears the old admission decision.
    NativeChildProcess::State observeRetainedChild(bool stop=false)noexcept{
        return decision_?decision_->observeRetainedChild(stop):NativeChildProcess::State::empty;
    }
};
}
