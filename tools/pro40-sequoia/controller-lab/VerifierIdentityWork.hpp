// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../NativeIdentityRequest.hpp"
#include "VerifierLaunchPolicy.hpp"
#include <utility>
namespace Pro40Lab {
// Staged original-peer verification work, not connected to the lifecycle protocol.
// The authenticated controller supplies the kernel-origin token. The helper's
// provisioned policy and current launch nonce are independent of request contents.
// This object neither authenticates that controller nor establishes token provenance.
class VerifierIdentityWork final {
    const std::array<uint8_t,20> policy_;
    const LaunchNonce nonce_;
    const uint64_t deadline_;
    bool consumed_=false;
public:
    VerifierIdentityWork(std::array<uint8_t,20> policy,LaunchNonce nonce,uint64_t deadline):
        policy_(policy),nonce_(nonce),deadline_(deadline){}
    VerifierIdentityWork(const VerifierIdentityWork&)=delete;
    VerifierIdentityWork& operator=(const VerifierIdentityWork&)=delete;
    // check(token) must use a separately provisioned Security requirement. It may
    // block; pre/post checks suppress late results, not preempt OS execution.
    template<class Now,class Stopped,class Check>
    [[nodiscard]] std::optional<Pro40Audio::NativeIdentityResult> execute(
        const Pro40Audio::NativeIdentityRequest& request,Now now,Stopped stopped,Check check){
        if(consumed_)return {};
        consumed_=true; // Consume even malformed/stopped requests; no retry.
        const auto began=now();
        if(!deadline_ || began>=deadline_ || stopped() || !request.valid() ||
           request.requestID!=nonce_ || request.policyHash!=policy_)return {};
        const auto owned=request; // Echo exactly what was checked, not later caller changes.
        const int32_t status=check(owned.token);
        const auto ended=now();
        if(ended<began || ended>=deadline_ || stopped())return {};
        return Pro40Audio::NativeIdentityResult{1,owned,status};
    }
};
}
