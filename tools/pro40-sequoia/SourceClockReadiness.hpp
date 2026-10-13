// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SourceFrameClock.hpp"
namespace Pro40Audio {
// Control-side structural readiness only. Does not authorize HAL publication:
// the estimator must separately establish continuity, wrap and fit quality.
struct SourceClockReadiness {
    struct Policy {
        uint64_t minimumPrefixFrames=0,initialReadFrames=0,capacityFrames=0;
        uint64_t maximumEvidenceAgeTicks=0;
    };
    struct Evidence {
        SourceFrameClock::Identity identity;
        uint64_t prefixFrames=0,readyHost=0;
        // Current contiguous retained PCM interval [firstFrame,endFrame).
        uint64_t firstFrame=0,endFrame=0;
    };
    enum class Result { invalidPolicy,invalidClock,wrongIdentity,invalidEvidence,
                        insufficientPrefix,initialHistoryLost,stale,structurallyReady };
    static Result check(const SourceFrameClock &clock,const Evidence &e,
                        const Policy &p,uint64_t now) noexcept {
        if(!p.minimumPrefixFrames || !p.initialReadFrames ||
           p.minimumPrefixFrames>p.capacityFrames || p.initialReadFrames>p.capacityFrames ||
           !p.maximumEvidenceAgeTicks)return Result::invalidPolicy;
        if(!clock.valid())return Result::invalidClock;
        if(e.identity!=clock.identity)return Result::wrongIdentity;
        // Subtractions follow ordered bounds; never add attacker-sized ranges.
        if(!e.readyHost || e.readyHost>now || e.readyHost<clock.origin ||
           !e.prefixFrames || e.prefixFrames>clock.horizonFrames ||
           e.firstFrame>e.endFrame || e.endFrame>clock.horizonFrames ||
           e.endFrame-e.firstFrame>p.capacityFrames || e.endFrame<e.prefixFrames)
            return Result::invalidEvidence;
        uint64_t lastHost=0,horizonHost=0;
        if(!clock.hostTime(e.identity,e.prefixFrames-1,lastHost) || lastHost>e.readyHost ||
           !clock.hostTime(e.identity,clock.horizonFrames,horizonHost) || now>horizonHost)
            return Result::invalidEvidence;
        if(e.prefixFrames<p.minimumPrefixFrames || e.endFrame<p.initialReadFrames)
            return Result::insufficientPrefix;
        if(e.firstFrame!=0)return Result::initialHistoryLost;
        if(now-e.readyHost>p.maximumEvidenceAgeTicks)return Result::stale;
        return Result::structurallyReady;
    }
};
}
