// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include "SustainedSession.hpp"
#include "StagedLockPoll.hpp"
namespace Pro40Packets {
// Acceptance policy for the explicitly supported bounded refill experiments.
// Linux dice-interface.h:134-168 defines clock/ARX lock and sticky slip bits.
class PlaybackLockCheck {
    unsigned budget_,acquisitionDeadline_;
    unsigned previous_=0,locked_=0,firstLocked_=0,lastLocked_=0;
    unsigned maximumAcceptedGap_=0,extendedStagedGaps_=0;
    bool seen_=false,acquired_=false,fault_=false;
public:
    static constexpr unsigned cadenceCycles=64;
    // Three quadlets per observation; include the initial acquisition prefix.
    static constexpr unsigned observationCapacity(unsigned budget) {
        return budget/cadenceCycles+8;
    }
    // Keep the existing256-cycle acquisition allowance AFTER the scheduled
    // no-data head. Lock cannot be expected before content reaches the device.
    // This is our bounded test policy, not a TCAT-specified lock timeout.
    explicit PlaybackLockCheck(unsigned budget=1024,unsigned contentStart=256):
        budget_(budget),acquisitionDeadline_(contentStart+256) {
        fault_=(contentStart!=256 && contentStart!=512) ||
            (budget!=1024 && budget!=4096 && budget!=8192 && budget!=32768 &&
             budget!=65536 && budget!=262144 && budget!=Pro40::sustainedCounterLimit);
    }
    bool observe(unsigned progress,uint32_t clock,uint32_t extended,uint32_t rate) {
        return observe(progress,clock,extended,rate,progress);
    }
    // A staged query begun by the acquisition deadline may finish within its
    // existing256-cycle span. Validate all fields only at completion. This is
    // a bounded observation allowance, not proof of the exact lock transition
    // time. Session-j_4w_nc7 read locked status in a query begun at717, but a
    // later11ms rate read delayed its completion to769 (deadline768).
    bool observe(unsigned progress,uint32_t clock,uint32_t extended,uint32_t rate,unsigned started) {
        if(fault_)return false;
        const bool both=(extended&0xc0)==0xc0;
        const unsigned gap=seen_ && progress>previous_?progress-previous_:0;
        // Steady monitoring may wait128 cycles before starting a query, which
        // itself has a256-cycle bound. Accept the combined gap only with proof
        // of an on-time start, after three locked observations. Session05hm4cx5:
        // previous12459, start12545, complete12783; all status fields healthy.
        // This is diagnostic policy, not a TCAT latency guarantee. The sticky
        // ARX slip bits still report errors since the last quadlet read
        // (references/linux-dice/dice-interface.h:152-168). No retry/new read.
        const bool boundedStagedGap=acquired_ && locked_>=3 && started>=previous_ &&
            started-previous_<=128 && started<=progress &&
            progress-started<=StagedLockPoll::maximumSpanCycles;
        if(started<128 || started>progress || progress-started>StagedLockPoll::maximumSpanCycles ||
           (seen_ && started<previous_) || progress>=budget_ ||
           (seen_ && (progress<=previous_ || (gap>256 && !boundedStagedGap))) ||
           (!seen_ && progress>=256) || clock!=0x201 || rate!=48000 ||
           (seen_ && (extended&0xc00000)) || (acquired_ && !both) ||
           (!acquired_ && started>acquisitionDeadline_) ||
           (progress>=acquisitionDeadline_ && !both)) {fault_=true;return false;}
        // First quadlet extended-status read clears any pre-baseline slip flags.
        if(gap>maximumAcceptedGap_)maximumAcceptedGap_=gap;
        if(gap>256)++extendedStagedGaps_;
        seen_=true;previous_=progress;
        if(both) {
            if(!acquired_)firstLocked_=progress;
            acquired_=true;++locked_;lastLocked_=progress;
        }
        return true;
    }
    // Staged acquisition already spent time collecting the last sample. Do not
    // add another completion-anchored gap before starting its successor. During
    // acquisition keep >=64 cycles between starts. A watermark may remain unchanged while
    // the next query begins; StagedLockPoll must wait for NEW completion progress
    // before taking its final timer sample. Acquisition/gap checks stay unchanged.
    unsigned nextStagedObservation(unsigned started) const {
        // Once locked, target the next64-cycle boundary, leaving one further
        // notification interval inside the unchanged128-cycle start allowance.
        // Nuendo session-bstovrcn: complete105600, old target105728 deferred for
        // TX bank work, actual start105729, complete105920 => inadmissible gap.
        // Target105664 instead. DMA still takes priority; delayed observations
        // must still pass observe(), with no invented or backdated progress.
        if(acquired_)return (previous_/cadenceCycles+1)*cadenceCycles;
        const unsigned cadence=started+cadenceCycles,after=previous_;
        return cadence>after?cadence:after;
    }
    unsigned lastObservation() const {return previous_;}
    unsigned firstLocked() const {return firstLocked_;}
    unsigned lastLocked() const {return lastLocked_;}
    unsigned lockedCount() const {return locked_;}
    unsigned maximumAcceptedGap() const {return maximumAcceptedGap_;}
    unsigned extendedStagedGaps() const {return extendedStagedGaps_;}
    bool validThrough(unsigned progress) const {
        return !fault_ && acquired_ && locked_>=3 && progress>=lastLocked_ &&
            progress-lastLocked_<=256;
    }
    bool complete() const {return !fault_ && acquired_ && locked_>=3 && lastLocked_>=budget_-256;}
};
}
