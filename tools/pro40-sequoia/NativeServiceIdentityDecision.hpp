// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeIdentityRequest.hpp"
#include "NativePeerCleanup.hpp"
#include <utility>
namespace Pro40Audio {
// Evidence supplied by a trusted pre-session supervisor. This value owns no XPC
// objects or process handles; unresolved resources remain owned by that supervisor.
// A reply/finalized connection is NEVER proof of remote exit. For this initial
// contract, remote completion requires independently observed service process exit.
enum class IdentityServiceEnd { unknown, notStarted, exitedClean, exitedFailed };
struct NativeServiceIdentityOutcome {
    NativeIdentityRequest expected;
    std::optional<NativeIdentityResult> reply;
    uint64_t deadline=0,observedAt=0; // Same monotonic unit/epoch, timeout at equality.
    bool submitted=false,authenticatedReply=false,cancelled=false;
    LocalCleanup local=LocalCleanup::incomplete;
    IdentityServiceEnd remote=IdentityServiceEnd::unknown;
};
// Immutable snapshot, no IO, launch, trust establishment, retry or block clearing.
// Separate from the pipe helper's clean-child-exit decision: no fabricated child.
class NativeServiceIdentityDecision {
    const NativeServiceIdentityOutcome outcome_;
    bool remoteClean()const noexcept {
        if(outcome_.remote==IdentityServiceEnd::exitedClean||outcome_.remote==IdentityServiceEnd::exitedFailed)return true;
        return outcome_.remote==IdentityServiceEnd::notStarted&&!outcome_.submitted&&!outcome_.reply&&!outcome_.authenticatedReply;
    }
public:
    explicit NativeServiceIdentityDecision(NativeServiceIdentityOutcome outcome)noexcept:outcome_(std::move(outcome)){}
    NativeServiceIdentityDecision(const NativeServiceIdentityDecision &)=delete;
    NativeServiceIdentityDecision &operator=(const NativeServiceIdentityDecision &)=delete;
    const NativeServiceIdentityOutcome &outcome()const noexcept{return outcome_;}
    std::optional<int32_t> boundStatus()const noexcept {
        if(!outcome_.submitted||!outcome_.authenticatedReply||outcome_.cancelled||
           !outcome_.deadline||outcome_.observedAt>=outcome_.deadline||!outcome_.expected.valid()||
           !outcome_.reply||outcome_.reply->version!=1||!(outcome_.reply->request==outcome_.expected))return std::nullopt;
        return outcome_.reply->status;
    }
    bool permitsAdmission()const noexcept {
        const auto status=boundStatus();
        return status&&*status==0&&outcome_.local==LocalCleanup::closed&&outcome_.remote==IdentityServiceEnd::exitedClean;
    }
    LocalCleanup combineLocalCleanup(LocalCleanup other)const noexcept {
        return outcome_.local==LocalCleanup::closed?other:LocalCleanup::incomplete;
    }
    RemoteCleanup combineRemoteCleanup(RemoteCleanup other)const noexcept {
        return remoteClean()?other:RemoteCleanup::unconfirmed;
    }
};
}
