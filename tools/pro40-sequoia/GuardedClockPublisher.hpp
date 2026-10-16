// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ClockEvidenceOrder.hpp"
#include "ClockUpdateCursor.hpp"
#include <optional>
namespace Pro40Audio {
// Offline single-owner composition; no raw cursor/evidence access is exposed.
// Structural validation only, NOT estimator quality admission or a HAL driver.
class GuardedClockPublisher {
public:
    using Clock=SourceFrameClock;using Stamp=Clock::Stamp;
    enum class Result {ready=0,published=0,waiting=1,invalid=2,identity=3,expiredClock=4,
        correctionTooLarge=5,backward=6,invalidEvidence=11,futureEvidence=12,
        staleEvidence=13,outOfOrderEvidence=14,expiredEvidence=15,unavailable=16,blocked=17,
        startupTimeout=18}; // Used by the owning native session, not this publisher.
    GuardedClockPublisher(uint64_t correction,uint64_t arrivalAge,uint64_t useAge)
        :order_(arrivalAge,useAge),correction_(correction){}
    Result offer(const Clock &clock,uint64_t sequence,uint64_t observed,uint64_t now) noexcept {
        if(failed_)return Result::blocked;
        if(!clock.valid())return Result::invalid;
        if(cursor_ && clock.identity!=proposal_.identity)return Result::identity;
        auto r=order_.accept(sequence,observed,now);
        if(r!=ClockEvidenceOrder::Result::accepted)return evidenceResult(r);
        proposal_=clock;
        if(!started_)cursor_.emplace(clock);
        return Result::ready;
    }
    Result advance(uint64_t now,Stamp &out) noexcept {
        if(failed_)return Result::blocked;
        auto r=order_.usable(now);
        if(r!=ClockEvidenceOrder::Result::accepted)return evidenceResult(r);
        if(!cursor_)return Result::unavailable;
        auto result=started_?cursor_->next(proposal_,now,correction_,out):cursor_->start(now,out);
        if(result==ClockUpdateCursor::Result::published)started_=true;
        else if(result!=ClockUpdateCursor::Result::waiting)failed_=true;
        return static_cast<Result>(result);
    }
    // Diagnostic historical value only. Never authorizes current clock use.
    bool lastCommitted(Stamp &out) const noexcept {return cursor_ && cursor_->last(out);}
private:
    static Result evidenceResult(ClockEvidenceOrder::Result r) noexcept {
        return static_cast<Result>(10+static_cast<int>(r));
    }
    ClockEvidenceOrder order_;std::optional<ClockUpdateCursor> cursor_;
    Clock proposal_{};uint64_t correction_;bool started_=false,failed_=false;
};
}
