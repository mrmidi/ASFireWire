// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../NativeServiceIdentityDecision.hpp"
namespace Pro40Lab {
// Typed terminal evidence from the retained session owner. Never reconstructed
// from UI/report dictionaries or XPC acknowledgements.
struct VerifierSessionCompletion {
    bool submitted=false,finished=false,cancelled=false;
    bool localClosed=false,registrationReconciled=false,helperCleanExitProven=false,callbackPending=false;
    uint64_t observedAt=0;
};
inline Pro40Audio::NativeServiceIdentityOutcome verifierIdentityOutcome(
    const Pro40Audio::NativeIdentityRequest& expected,
    std::optional<Pro40Audio::NativeIdentityResult> authenticatedResult,
    uint64_t deadline,uint64_t now,bool stopped,
    const std::optional<VerifierSessionCompletion>& completion){
    Pro40Audio::NativeServiceIdentityOutcome result;
    result.expected=expected;result.reply=std::move(authenticatedResult);
    result.authenticatedReply=result.reply.has_value();result.deadline=deadline;result.observedAt=now;
    result.cancelled=stopped || (completion && completion->cancelled);
    if(completion){
        if(!completion->observedAt || now<completion->observedAt)result.deadline=0;
        result.submitted=completion->submitted;
        if(completion->finished && !completion->callbackPending){
            if(completion->localClosed)result.local=Pro40Audio::LocalCleanup::closed;
            if(completion->submitted && completion->registrationReconciled && completion->helperCleanExitProven)
                result.remote=Pro40Audio::IdentityServiceEnd::exitedClean;
        }
    }
    return result; // Unknown remains unknown; no inferred not-started/failed exit.
}
}
