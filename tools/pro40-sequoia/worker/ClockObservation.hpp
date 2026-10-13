// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../NativeClockObserver.hpp"
#include "RecordingExport.hpp"
#include "StartupObservation.hpp"
#include "InputSessionObservation.hpp"
#include <mach/mach_time.h>
#include <optional>
namespace Pro40 {
// Attach before the synchronous run; detach/export only after it returns.
// Experimental diagnostics only. Failure never changes native status or lease.
struct ObservationHostClock {
    static uint64_t now() noexcept {return mach_absolute_time();}
};
// The clock type is a compile-time test seam for saved-evidence replay; live
// aliases use Mach time. Measured callback costs always use real Mach time.
template<class HostClock=ObservationHostClock>
class BasicClockObservation {
    NativeTimingEvidence &timing_;
    std::optional<Pro40Audio::NativeClockObserver> observer_;
    std::optional<StartupObservation> startup_;
    InputSessionObservation *input_=nullptr; // Optional caller-owned sink; outlives attachment.
    uint64_t maximumCost_=0,calls_=0;bool failedBeforeStart_=false;
    struct Cost {uint64_t calls=0,maximum=0;};
    // Entry phase: disabled, waiting, collecting, completed, failed. The call
    // that completes startup still belongs to collecting, never steady state.
    std::array<Cost,5> costs_{};
    uint64_t ticks(uint64_t ns) const noexcept {return uint64_t(__uint128_t(ns)*timing_.denom/timing_.numer);}
    template<class F> void measured(F f) noexcept {
        const auto phase=startup_?size_t(startup_->state())+1:0;
        const auto begin=mach_absolute_time();f();const auto cost=mach_absolute_time()-begin;
        if(cost>maximumCost_)maximumCost_=cost;++calls_;
        auto &bucket=costs_[phase];++bucket.calls;
        if(cost>bucket.maximum)bucket.maximum=cost;
    }
    void failure() noexcept {
        if(input_)input_->failed();
        if(startup_)startup_->failed();
        if(observer_)observer_->recorderFailed();else failedBeforeStart_=true;
    }
public:
    explicit BasicClockObservation(NativeTimingEvidence &timing,const Pro40Packets::InputRecording *recording=nullptr,
                              InputSessionObservation *input=nullptr):timing_(timing),input_(input) {
        if(recording)startup_.emplace(*recording,timing.numer,timing.denom);
        timing_.packets.observer={this,
            [](void *ctx,size_t ordinal,const Pro40Packets::InputTimingEvidence<>::Record &p) noexcept {
                auto &s=*static_cast<BasicClockObservation *>(ctx);
                s.measured([&]{
                    if(s.observer_)s.observer_->packet(ordinal,p);else s.failure();
                    if(s.startup_)s.startup_->packet(ordinal,p);
                    if(s.input_)s.input_->packet(ordinal,p,HostClock::now());
                });
            },[](void *ctx) noexcept {
                auto &s=*static_cast<BasicClockObservation *>(ctx);s.measured([&]{s.failure();});
            }};
        timing_.observerContext=this;
        timing_.clockObserver=[](void *ctx,const NativeTimingEvidence::Clock &p,bool fault) noexcept {
            auto &s=*static_cast<BasicClockObservation *>(ctx);
            s.measured([&]{
                if(!s.observer_ && !s.failedBeforeStart_ && !fault && p.beforeReceive && s.timing_.numer && s.timing_.denom){
                    s.observer_.emplace(Pro40Audio::SourceFrameClock::Identity{1,1,p.generation,s.timing_.numer,s.timing_.denom},
                        2880000,s.ticks(1000),s.ticks(50000000),s.ticks(1000000000),
                        Pro40Audio::NativeClockSchedule::Policy{4096,32},
                        Pro40Audio::NativeClockSession::Startup{p.before,s.ticks(4000000000)},s.ticks(250000000));
                }
                if(fault)s.failure();
                else if(s.observer_)s.observer_->clock(p);
                else s.failure();
                if(s.observer_)s.observer_->poll(HostClock::now());
                if(s.startup_)s.startup_->clock(p,HostClock::now(),fault);
                if(s.input_)s.input_->clock(p,HostClock::now(),fault);
            });
        };
        timing_.pollObserver=[](void *ctx) noexcept {
            auto &s=*static_cast<BasicClockObservation *>(ctx);
            s.measured([&]{
                if(s.observer_)s.observer_->poll(HostClock::now());
                if(s.startup_)s.startup_->poll(HostClock::now());
                if(s.input_)s.input_->poll(HostClock::now());
            });
        };
    }
    BasicClockObservation(const BasicClockObservation &)=delete;
    BasicClockObservation &operator=(const BasicClockObservation &)=delete;
    ~BasicClockObservation(){timing_.packets.observer={};timing_.observerContext=nullptr;timing_.clockObserver=nullptr;timing_.pollObserver=nullptr;}
    bool exportStartupReport(const std::string &log,const NativeSessionResult &native){
        return startup_&&startup_->exportReport(log,native);
    }
    bool exportReport(const std::string &log,const NativeSessionResult &native) {
        const auto r=observer_?observer_->report():Pro40Audio::NativeClockObserver::Report{};
        if(observer_)observer_->stop();
        char text[1800];
        const int n=std::snprintf(text,sizeof(text),
            "{\"schema\":2,\"diagnostic_only\":true,\"started\":%s,\"prestart_fault\":%s,\"native_accepted\":%s,\"state\":%d,\"session_result\":%d,\"packets\":%llu,\"pairs\":%llu,\"polls\":%llu,\"proposals\":%llu,\"stamps\":%llu,\"last_frame\":%.0f,\"last_host\":%llu,\"max_poll_gap_ticks\":%llu,\"max_stamp_lag_ticks\":%llu,\"first_publication_delay_ticks\":%llu,\"max_observer_cost_ticks\":%llu,\"measured_calls\":%llu,\"numer\":%u,\"denom\":%u,\"post_start_polls\":%llu,\"max_post_start_poll_gap_ticks\":%llu,\"max_post_start_stamp_lag_ticks\":%llu,\"first_stamp_lag_ticks\":%llu}\n",
            observer_?"true":"false",failedBeforeStart_?"true":"false",native.accepted()?"true":"false",int(r.state),r.sessionResult,
            (unsigned long long)r.packets,(unsigned long long)r.pairs,(unsigned long long)r.polls,(unsigned long long)r.proposals,
            (unsigned long long)r.stamps,r.last.sample,(unsigned long long)r.last.host,(unsigned long long)r.maximumPollGap,
            (unsigned long long)r.maximumStampLag,(unsigned long long)r.firstPublicationDelay,(unsigned long long)maximumCost_,
            (unsigned long long)calls_,timing_.numer,timing_.denom,(unsigned long long)r.postStartPolls,
            (unsigned long long)r.maximumPostStartPollGap,(unsigned long long)r.maximumPostStartStampLag,
            (unsigned long long)r.firstStampLag);
        if(n<=0||size_t(n)>=sizeof(text))return false;
        // Assemble only after teardown; no formatting/IO in the measured path.
        std::string report(text,size_t(n)-2); // Remove closing brace and newline.
        report+=",\"cost_phase_schema\":1,\"cost_phases\":{";
        constexpr const char *names[]={"disabled","waiting","collecting","completed","failed"};
        for(size_t i=0;i<costs_.size();++i){
            char item[160];const int count=std::snprintf(item,sizeof(item),
                "%s\"%s\":{\"calls\":%llu,\"maximum_ticks\":%llu}",i?",":"",names[i],
                (unsigned long long)costs_[i].calls,(unsigned long long)costs_[i].maximum);
            if(count<=0||size_t(count)>=sizeof(item))return false;
            report.append(item,size_t(count));
        }
        report+="}}\n";
        return publishRecordingFile(log+".clock-observer.json",{reinterpret_cast<const uint8_t *>(report.data()),report.size()});
    }
};
using ClockObservation=BasicClockObservation<>;
}
