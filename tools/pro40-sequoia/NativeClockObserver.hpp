// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeClockSession.hpp"
#include "NativeTimingEvidence.hpp"
namespace Pro40Audio {
// Diagnostic adapter only. One control owner, caller-owned lifetime, no IO.
// Consume each recorder record once. Report recorder failure even if no new
// record was appended. Poll independently of packet arrival; one stamp per poll.
class NativeClockObserver {
public:
    enum class State {observing,inputFault,recorderFault,sessionFault,lateStamp,stopped};
    struct Report {
        State state=State::observing;int sessionResult=0;
        uint64_t packets=0,pairs=0,polls=0,proposals=0,stamps=0;
        uint64_t maximumPollGap=0,maximumStampLag=0,firstPublicationDelay=0;
        // Post-start means the poll began after a first stamp was accepted.
        uint64_t postStartPolls=0,maximumPostStartPollGap=0,maximumPostStartStampLag=0,firstStampLag=0;
        SourceFrameClock::Stamp last{};
    };
    NativeClockObserver(SourceFrameClock::Identity id,uint64_t horizon,uint64_t correction,
                        uint64_t arrivalAge,uint64_t useAge,NativeClockSchedule::Policy schedule,
                        NativeClockSession::Startup startup,uint64_t maxStampLag)
        :session_(id,horizon,correction,arrivalAge,useAge,schedule,startup),
         start_(startup.startedHost),lastPoll_(startup.startedHost),maxLag_(maxStampLag){
        if(!maxLag_)fail(State::sessionFault,int(NativeClockSession::Result::invalid));
    }
    bool packet(uint64_t ordinal,const Pro40Packets::InputTimingEvidence<>::Record &p) noexcept {
        if(!active())return false;
        if(!session_.packet({ordinal,p.firstFrame,p.generation,p.arrivalCycle,p.syt,p.blocks,p.dbc}))
            return fail(State::inputFault);
        ++report_.packets;return true;
    }
    bool clock(const Pro40::NativeTimingEvidence::Clock &p) noexcept {
        if(!active())return false;
        if(!session_.clock({p.before,p.uptime,p.after,p.generation,p.cycle,p.status,p.packetCount,p.beforeReceive}))
            return fail(State::inputFault);
        ++report_.pairs;return true;
    }
    void recorderFailed() noexcept {if(active())fail(State::recorderFault);}
    void stop() noexcept {if(active())fail(State::stopped);}
    bool poll(uint64_t now) noexcept {
        if(!active())return false;
        if(now<lastPoll_||report_.polls==UINT64_MAX)
            return fail(State::sessionFault,int(NativeClockSession::Result::outOfOrderEvidence));
        const auto gap=now-lastPoll_;if(gap>report_.maximumPollGap)report_.maximumPollGap=gap;
        const bool postStart=report_.stamps!=0;
        if(postStart){
            ++report_.postStartPolls;
            if(gap>report_.maximumPostStartPollGap)report_.maximumPostStartPollGap=gap;
        }
        lastPoll_=now;++report_.polls;
        using R=NativeClockSession::Result;
        const auto offer=session_.propose(now);
        if(offer==R::ready)++report_.proposals;
        else if(offer!=R::waiting)return fail(State::sessionFault,int(offer));
        SourceFrameClock::Stamp stamp;
        const auto result=session_.advance(now,stamp);
        if(result==R::waiting||result==R::unavailable)return true;
        if(result!=R::published)return fail(State::sessionFault,int(result));
        const auto lag=now-stamp.host;
        if(lag>report_.maximumStampLag)report_.maximumStampLag=lag;
        if(postStart&&lag>report_.maximumPostStartStampLag)report_.maximumPostStartStampLag=lag;
        // No replay catch-up loop or silent skip over old sample boundaries.
        if(lag>maxLag_)return fail(State::lateStamp);
        if(!report_.stamps){report_.firstPublicationDelay=now-start_;report_.firstStampLag=lag;}
        ++report_.stamps;report_.last=stamp;return true;
    }
    Report report() const noexcept {return report_;} // Same owner, or after quiescence.
private:
    bool active() const noexcept {return report_.state==State::observing;}
    bool fail(State state,int result=0) noexcept {
        report_.state=state;report_.sessionResult=result;session_.stop();return false;
    }
    NativeClockSession session_;Report report_;
    uint64_t start_,lastPoll_,maxLag_;
};
}
