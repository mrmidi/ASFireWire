// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SubmittedOwnershipCollector.hpp"
namespace Pro40Lab {
// One bounded failure summary, never raw BTM contents or hot-path telemetry.
inline NSString *submittedCollectionFailureDetail(const SubmittedCollectionResult& r){
    const char *status="unknown";
    switch(r.status){
        case SubmittedCollectionStatus::complete:status="missing-observation";break;
        case SubmittedCollectionStatus::cancelled:status="cancelled";break;
        case SubmittedCollectionStatus::expired:status="expired";break;
        case SubmittedCollectionStatus::invalidTime:status="invalid-time";break;
        case SubmittedCollectionStatus::invalidTarget:status="invalid-target";break;
        case SubmittedCollectionStatus::btmFailed:status="btm-failed";break;
        case SubmittedCollectionStatus::jobFailed:status="job-failed";break;
    }
    auto describe=[](const JobProcessResult& p){
        return [NSString stringWithFormat:@"exit=%d,reaped=%d,timeout=%d,overflow=%d,io=%d,spawn=%d,stderr=%zu",
            p.exit,int(p.reaped),int(p.timedOut),int(p.overflow),int(p.ioError),p.spawnError,p.err.size()];
    };
    return [NSString stringWithFormat:@"ownership collection incomplete: status=%s; btm{%@}; job{%@}",
        status,describe(r.btm),describe(r.job)];
}
// Called on the resources' serial owner queue, once after successful registration.
// A refusal never performs removal or prepares/activates a connection.
template<class Service,class Collect,class Now,class Stop>
bool prepareSubmittedOwnership(VerifierSessionResources<Service>& resources,Collect collect,Now now,Stop stop){
    if(!resources.launch || !resources.claimOwnershipCollection())return false;
    const auto began=now();
    auto fail=[&](NSString *category,NSString *detail){resources.launch->abandon(category,detail,now());return false;};
    if(stop())return fail(@"cancelled",@"ownership collection cancelled before query");
    if(!began || began>=resources.deadline())return fail(@"expired",@"ownership collection deadline unavailable");
    auto result=collect();
    const auto ended=now();
    if(stop() || result.status==SubmittedCollectionStatus::cancelled)return fail(@"cancelled",@"ownership collection cancelled");
    if(ended<began)return fail(@"observation-invalid",@"ownership collection clock moved backwards");
    if(ended>=resources.deadline() || resources.deadline()-ended<=submittedSessionReserveNS ||
       result.status==SubmittedCollectionStatus::expired)return fail(@"expired",@"ownership collection exhausted startup budget");
    if(result.status!=SubmittedCollectionStatus::complete || !result.observation)return fail(@"observation-invalid",submittedCollectionFailureDetail(result));
    if(result.observation->began<began)return fail(@"observation-invalid",@"ownership collection predates startup stage");
    auto proof=SubmittedVerifierOwnership::bind(resources,*result.observation,ended);
    if(!proof || !resources.launch->bindOwnership(*proof))return fail(@"observation-invalid",@"Submitted ownership binding rejected");
    return true;
}
// Registration preparation retains its own 5s cap. 25s leaves room for that cap,
// 8s collection, the 8s session reserve and 4s admission slack. Host exit is later.
inline constexpr uint64_t nativeVerifierDeadlineNS=25000000000ULL;
inline constexpr auto nativeVerifierCommandLimit=std::chrono::seconds(30);
static_assert(nativeVerifierDeadlineNS>=5000000000ULL+submittedCollectionNS+submittedSessionReserveNS);
}
