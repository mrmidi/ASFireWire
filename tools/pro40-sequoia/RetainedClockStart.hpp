// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioWindowPublication.hpp"
#include "SourceClockReadiness.hpp"
namespace Pro40Audio {
// Structural startup reservation, not clock-quality or HAL admission. Owns a
// pinned immutable PCM window; publisher must outlive this object and all calls.
// Owner must separately cancel/quiesce on stop or generation change.
// Like the underlying lease, a moved-from object must not be read. This is a
// structural reservation: the owning session supplies monotonic use times and
// terminal expiry/cancellation. It is not a replacement for that session gate.
template<size_t Capacity> class RetainedClockStart {
    using Publication=AudioWindowPublication<Capacity>;
    using Gate=SourceClockReadiness;
public:
    enum class Result {retained,unavailable,notReady,invalidRead};
    struct Evidence {SourceFrameClock::Identity identity;uint64_t prefixFrames=0,readyHost=0;};
    static std::optional<RetainedClockStart> acquire(Publication &publication,
            const SourceFrameClock &clock,const Evidence &e,const Gate::Policy &policy,
            uint64_t now,Result &result,Gate::Result &reason) noexcept {
        result=Result::notReady;
        if(policy.capacityFrames!=Capacity){reason=Gate::Result::invalidPolicy;return std::nullopt;}
        auto lease=publication.tryAcquire();
        if(!lease){result=Result::unavailable;return std::nullopt;}
        if(lease->epoch()!=e.identity.epoch){reason=Gate::Result::wrongIdentity;return std::nullopt;}
        Gate::Evidence held{e.identity,e.prefixFrames,e.readyHost,lease->oldest(),lease->next()};
        reason=Gate::check(clock,held,policy,now);
        if(reason!=Gate::Result::structurallyReady)return std::nullopt;
        result=Result::retained;
        return RetainedClockStart(std::move(*lease),clock,held,policy);
    }
    RetainedClockStart(const RetainedClockStart &)=delete;
    RetainedClockStart &operator=(const RetainedClockStart &)=delete;
    RetainedClockStart(RetainedClockStart &&)=default;
    RetainedClockStart &operator=(RetainedClockStart &&)=default;
    Result readInitial(uint64_t now,std::span<int32_t> output) const noexcept {
        // Recheck evidence age at use, while still holding the same PCM lease.
        if(output.size()!=policy_.initialReadFrames){std::fill(output.begin(),output.end(),0);return Result::invalidRead;}
        if(Gate::check(clock_,evidence_,policy_,now)!=Gate::Result::structurallyReady){
            std::fill(output.begin(),output.end(),0);return Result::notReady;
        }
        return lease_.read(evidence_.identity.epoch,0,output)==WindowResult::ok?Result::retained:Result::notReady;
    }
private:
    RetainedClockStart(typename Publication::Lease lease,SourceFrameClock clock,
                      Gate::Evidence evidence,Gate::Policy policy)
        :lease_(std::move(lease)),clock_(clock),evidence_(evidence),policy_(policy){}
    typename Publication::Lease lease_;SourceFrameClock clock_;
    Gate::Evidence evidence_;Gate::Policy policy_;
};
}
