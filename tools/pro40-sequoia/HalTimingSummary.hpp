// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "HalCallbackTiming.hpp"
#include <algorithm>
#include <cmath>
namespace Pro40Audio {
// Finite test-profile evidence, consumed only under HalTimingCollector's
// non-RT lock. Keep earlier groups to detect late records across Stop/Start.
// Overflow invalidates the evidence rather than silently dropping a group.
template<size_t Capacity> class HalTimingSummary {
public:
    struct Group {
        uint64_t seed=0,records=0,frames=0,faults=0;
        uint64_t anchor=0,firstHost=0,lastHost=0,lastPresentation=0;
        uint32_t client=0,operation=0,numer=0,denom=0,minFrames=0,maxFrames=0;
        double firstSample=0,lastSampleEnd=0,minLead=0,maxLead=0;
        bool emitted=false;
    };
private:
    std::array<Group,Capacity> groups_{};
    size_t size_=0;
    uint64_t invalid_=0,overflow_=0,records_=0;
public:
    void record(const HalCallbackTimingRecord &r) noexcept {
        ++records_;
        const bool input=r.operation==kAudioServerPlugInIOOperationReadInput;
        const auto &time=input?r.cycle.mInputTime:r.cycle.mOutputTime;
        // Stay within double's exact-integer range, including the end frame.
        if((!input && r.operation!=kAudioServerPlugInIOOperationWriteMix) ||
           !r.seed || !r.numer || !r.denom || r.host<r.anchor ||
           !r.frames || r.frames>4096 || (time.mFlags&3)!=3 ||
           !std::isfinite(time.mSampleTime) || std::trunc(time.mSampleTime)!=time.mSampleTime ||
           std::abs(time.mSampleTime)>9007199254736896.0) {++invalid_;return;}
        Group *group=nullptr;
        for(size_t i=0;i<size_;++i)if(groups_[i].seed==r.seed &&
            groups_[i].client==r.client && groups_[i].operation==r.operation){group=&groups_[i];break;}
        if(!group) {
            if(size_==Capacity){++overflow_;return;}
            group=&groups_[size_++];group->seed=r.seed;group->client=r.client;group->operation=r.operation;
            group->anchor=r.anchor;group->numer=r.numer;group->denom=r.denom;
            group->firstSample=time.mSampleTime;group->firstHost=r.host;group->minFrames=r.frames;
        }
        auto &g=*group;
        if(g.emitted || g.anchor!=r.anchor || g.numer!=r.numer || g.denom!=r.denom ||
           (g.records && (time.mSampleTime!=g.lastSampleEnd || r.host<g.lastHost ||
                         time.mHostTime<=g.lastPresentation)))++g.faults;
        const double delta=time.mHostTime>=r.host?double(time.mHostTime-r.host):-double(r.host-time.mHostTime);
        const double lead=delta*r.numer/r.denom/1e6;
        g.minLead=g.records?std::min(g.minLead,lead):lead;
        g.maxLead=g.records?std::max(g.maxLead,lead):lead;
        ++g.records;g.frames+=r.frames;g.lastSampleEnd=time.mSampleTime+r.frames;
        g.lastHost=r.host;g.lastPresentation=time.mHostTime;
        g.minFrames=std::min(g.minFrames,r.frames);g.maxFrames=std::max(g.maxFrames,r.frames);
    }
    // Repeat prior groups verbatim on later Stop; any late record changes the
    // group and adds a fault. The analyzer rejects changed repeated summaries.
    template<class Consumer> void emit(Consumer &&consume) {
        for(size_t i=0;i<size_;++i){consume(groups_[i]);groups_[i].emitted=true;}
    }
    uint64_t records() const noexcept{return records_;}
    uint64_t invalid() const noexcept{return invalid_;}
    uint64_t overflow() const noexcept{return overflow_;}
};
}
