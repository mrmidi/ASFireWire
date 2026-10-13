// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SourceFrameClock.hpp"
namespace Pro40Audio {
// Immutable-by-copy control value from the authoritative startup transaction.
// No IPC ABI, fit-quality approval or claim that the entire prefix is retained
// by the recipient. Only initialFrames samples transfer with this descriptor.
struct NativeStartDescriptor {
    SourceFrameClock clock;
    uint64_t evidenceSequence=0,prefixFrames=0,evidenceHost=0;
    uint64_t preparedHost=0,expiresHost=0,initialFrames=0;
    bool validAt(uint64_t now) const noexcept {
        uint64_t last=0,horizon=0;
        return clock.valid()&&evidenceSequence&&initialFrames&&initialFrames%8==0&&
            initialFrames<=prefixFrames&&prefixFrames<=clock.horizonFrames&&
            evidenceHost>=clock.origin&&preparedHost>=evidenceHost&&
            expiresHost>=preparedHost&&now>=preparedHost&&now<=expiresHost&&
            clock.hostTime(clock.identity,prefixFrames-1,last)&&last<=evidenceHost&&
            clock.hostTime(clock.identity,clock.horizonFrames,horizon)&&expiresHost<=horizon;
    }
    bool operator==(const NativeStartDescriptor &other) const noexcept {
        const auto &a=clock;const auto &b=other.clock;
        return a.identity==b.identity&&a.origin==b.origin&&a.ticksNumerator==b.ticksNumerator&&
            a.ticksDenominator==b.ticksDenominator&&a.horizonFrames==b.horizonFrames&&
            evidenceSequence==other.evidenceSequence&&prefixFrames==other.prefixFrames&&
            evidenceHost==other.evidenceHost&&preparedHost==other.preparedHost&&
            expiresHost==other.expiresHost&&initialFrames==other.initialFrames;
    }
};
}
