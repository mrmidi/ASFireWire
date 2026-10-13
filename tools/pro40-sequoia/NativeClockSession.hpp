// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeClockProposal.hpp"
#include "GuardedClockPublisher.hpp"
#include "NativeClockSchedule.hpp"
namespace Pro40Audio {
// Offline single-owner composition. No live HAL, IO, locks or quality approval.
// A terminal input/publication fault blocks every future use of this session.
class NativeClockSession {
public:
    using Result=GuardedClockPublisher::Result;
    using Stamp=SourceFrameClock::Stamp;
    struct Startup {uint64_t startedHost=0,timeoutTicks=0;};
    NativeClockSession(SourceFrameClock::Identity identity,uint64_t horizon,
                       uint64_t correction,uint64_t arrivalAge,uint64_t useAge,
                       NativeClockSchedule::Policy schedule,Startup startup)
        :fit_(identity.generation,identity.numer,identity.denom),
         publisher_(correction,arrivalAge,useAge),schedule_(schedule),identity_(identity),horizon_(horizon),
         startup_(startup),lastPoll_(startup.startedHost),
         blocked_(!schedule.minimumFrames||!schedule.pairsPerUpdate||!startup.startedHost||
                  !startup.timeoutTicks||startup.timeoutTicks>UINT64_MAX-startup.startedHost){}
    bool packet(const PacketClockFit::Packet &p) noexcept {
        if(blocked_)return false;
        if(!fit_.packet(p)){blocked_=true;return false;}return true;
    }
    bool clock(const ControllerClockFit::Pair &p) noexcept {
        if(blocked_)return false;
        if(!fit_.clock(p)){blocked_=true;return false;}
        observed_=p.after;++sequence_;return true;
    }
    // Poll after each paired read. Selection is internal; duplicate polls wait.
    // Waiting does not authorize using old timestamps: advance checks expiry.
    Result propose(uint64_t now) noexcept {
        const auto check=checkTime(now);if(check!=Result::ready)return check;
        NativeClockFit::Snapshot s;
        // Incomplete startup or packets beyond last paired read: not a fault.
        if(!fit_.snapshot(s))return Result::waiting;
        const auto decision=schedule_.observe(s.bus.count,s.packet.count*8);
        if(decision==NativeClockSchedule::Result::waiting)return Result::waiting;
        if(decision==NativeClockSchedule::Result::invalid){blocked_=true;return Result::invalid;}
        SourceFrameClock c;
        if(!nativeClockProposal(s,identity_,horizon_,c)){blocked_=true;return Result::invalid;}
        const auto r=publisher_.offer(c,sequence_,observed_,now);
        if(r!=Result::ready)blocked_=true;
        return r;
    }
    Result advance(uint64_t now,Stamp &out) noexcept {
        const auto check=checkTime(now);if(check!=Result::ready)return check;
        const auto r=publisher_.advance(now,out);
        if(r==Result::published)started_=true;
        if(r!=Result::published && r!=Result::waiting && r!=Result::unavailable)blocked_=true;
        return r;
    }
    void stop() noexcept {blocked_=true;}
    // Historical diagnostic only; callers cannot access the underlying publisher.
    bool lastCommitted(Stamp &out) const noexcept {return publisher_.lastCommitted(out);}
private:
    // Owner must poll even if no packets arrive. No hidden timer or IO here.
    // Deadline includes first publication, not merely receipt of a proposal.
    Result checkTime(uint64_t now) noexcept {
        if(blocked_)return Result::blocked;
        if(now<lastPoll_){blocked_=true;return Result::outOfOrderEvidence;}
        lastPoll_=now;
        if(!started_&&now-startup_.startedHost>startup_.timeoutTicks){
            blocked_=true;return Result::startupTimeout;
        }
        return Result::ready;
    }
    NativeClockFit fit_;GuardedClockPublisher publisher_;NativeClockSchedule schedule_;
    SourceFrameClock::Identity identity_;uint64_t horizon_,observed_=0,sequence_=0;
    Startup startup_;uint64_t lastPoll_=0;
    bool started_=false,blocked_=false;
};
}
