// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeIdentityRequest.hpp"
#include "NativeSessionRunner.hpp"
#include <utility>
namespace Pro40Audio {
// Synchronous source-runner binding. Epoch is the controller session epoch,
// not a FireWire bus generation. Owns option values, not their borrowed contexts.
// Original peer, recording/timing/hook contexts and stop owner must outlive run.
class NativeVerifiedRunnerContinuation final {
    NativeIdentityRequest expected_;
    Pro40::NativeSessionOptions options_;
    uint64_t epoch_;
    bool consumed_=false;
public:
    NativeVerifiedRunnerContinuation(NativeIdentityRequest expected,
        Pro40::NativeSessionOptions options,uint64_t epoch):
        expected_(expected),options_(std::move(options)),epoch_(epoch){}
    NativeVerifiedRunnerContinuation(const NativeVerifiedRunnerContinuation&)=delete;
    NativeVerifiedRunnerContinuation& operator=(const NativeVerifiedRunnerContinuation&)=delete;
    template<class Runner>
    [[nodiscard]] Pro40::NativeSessionResult run(Pro40::StopQuery stop,Runner&& runner){
        if(consumed_)return {75,{}};
        consumed_=true;
        if(!expected_.valid() || expected_.role!=1 || options_.guid!=expected_.guid ||
           epoch_!=expected_.epoch)return {64,{}};
        // The gate's live query is authoritative, not a stale copied option.
        options_.stop=stop;
        // Runner owns option validation, stop handling, lease acquisition and IO.
        // Preserve even missing outcomes/errors; never synthesize success here.
        return std::forward<Runner>(runner)(options_);
    }
};
}
