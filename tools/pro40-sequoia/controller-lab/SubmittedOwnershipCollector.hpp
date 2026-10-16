// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SubmittedVerifierOwnership.hpp"
#include <algorithm>
#ifdef PRO40_ENABLE_DUPLEX_DELEGATED
#include "AuthenticatedOwnershipObservation.hpp"
#endif
namespace Pro40Lab {
enum class SubmittedCollectionStatus { complete,cancelled,expired,invalidTime,invalidTarget,btmFailed,jobFailed };
struct SubmittedCollectionResult {
    SubmittedCollectionStatus status=SubmittedCollectionStatus::expired;
    // Failed/cancelled work has diagnostics but never publishable evidence.
    JobProcessResult btm,job;
    std::optional<SubmittedOwnershipObservation> observation;
};
// Budgets are admission limits, not promises about kernel scheduling/reaping.
// Keep the whole observation below the proof factory's ten-second freshness bound.
inline constexpr uint64_t submittedCollectionNS=8000000000ULL;
inline constexpr uint64_t submittedSessionReserveNS=8000000000ULL;
template<class Run,class Now,class Stop>
SubmittedCollectionResult collectSubmittedOwnership(const std::string& target,uint64_t deadline,
    Run run,Now now,Stop stop){
    SubmittedCollectionResult result;
    if(target.empty() || target.find('\0')!=std::string::npos){result.status=SubmittedCollectionStatus::invalidTarget;return result;}
    const auto began=now();auto last=began;
    if(!began){result.status=SubmittedCollectionStatus::invalidTime;return result;}
    auto checkpoint=[&](bool initial=false){
        const auto current=now();
        if(current<last){result.status=SubmittedCollectionStatus::invalidTime;return false;}
        last=current;
        if(stop()){result.status=SubmittedCollectionStatus::cancelled;return false;}
        if(current>=deadline || deadline-current<=submittedSessionReserveNS ||
           current-began>=submittedCollectionNS ||
           (initial && deadline-current<submittedCollectionNS+submittedSessionReserveNS)){
            result.status=SubmittedCollectionStatus::expired;return false;
        }
        return true;
    };
    if(!checkpoint(true))return result;
    auto budget=[&](uint64_t cap){
        return std::chrono::milliseconds(std::min({cap,submittedCollectionNS-(last-began),
            deadline-last-submittedSessionReserveNS})/1000000ULL);
    };
    // One BTM query, then a fresh job query. Never poll BTM in a session callback.
    result.btm=run(std::vector<std::string>{"/usr/bin/sfltool","dumpbtm"},budget(7000000000ULL),size_t(1048576));
    if(!checkpoint())return result;
    if(!result.btm.usable() || result.btm.exit!=0 || !result.btm.err.empty()){
        result.status=SubmittedCollectionStatus::btmFailed;return result;
    }
    const auto jobBudget=budget(1000000000ULL);
    if(jobBudget.count()<=0){result.status=SubmittedCollectionStatus::expired;return result;}
    result.job=run(std::vector<std::string>{"/bin/launchctl","print",target},jobBudget,size_t(16384));
    if(!checkpoint())return result;
    if(!result.job.usable() || result.job.exit!=0 || !result.job.err.empty()){
        result.status=SubmittedCollectionStatus::jobFailed;return result;
    }
    result.observation=SubmittedOwnershipObservation{result.btm,result.job,began,last};
    result.status=SubmittedCollectionStatus::complete;return result;
}
// Read-only native adapter; caller still needs the registered-resource proof
// factory and journal bind. This function neither registers nor starts a helper.
template<class Stop>
SubmittedCollectionResult collectNativeSubmittedOwnership(const std::string& target,uint64_t deadline,Stop stop){
    return collectSubmittedOwnership(target,deadline,
        [](const std::vector<std::string>& args,std::chrono::milliseconds budget,size_t limit){
#ifdef PRO40_ENABLE_DUPLEX_DELEGATED
            if(args==std::vector<std::string>{"/usr/bin/sfltool","dumpbtm"}) {
                // Three seconds for the private RPC plus two for mutual code
                // introduction fit inside the original seven-second cap.
                // Failure closes admission; never run an interactive fallback.
                if(budget<std::chrono::seconds(5) || limit!=1048576){JobProcessResult r;r.ioError=true;return r;}
                return authenticatedOwnershipObservation();
            }
#endif
            return observeProcess(args,budget,limit);
        },[]{return observationNS();},std::move(stop));
}
}
