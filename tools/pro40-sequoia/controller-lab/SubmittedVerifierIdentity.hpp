#include "LabIdentity.hpp"
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "VerifierJobSnapshot.hpp"
namespace Pro40Lab {
template<class Service> class VerifierSessionResources;
struct SubmittedOwnershipObservation;
// Only the registration/BTM factory can construct this proof. Keep its value
// contract independent of the resource owner to avoid an ownership include cycle.
class SubmittedVerifierOwnership final {
    std::string uuid_,target_,bundle_,attempt_;
    uint64_t observed_=0,deadline_=0;
    SubmittedVerifierOwnership(std::string uuid,std::string target,std::string bundle,
        uint64_t observed,uint64_t deadline,std::string attempt):uuid_(std::move(uuid)),target_(std::move(target)),
        bundle_(std::move(bundle)),attempt_(std::move(attempt)),observed_(observed),deadline_(deadline){}
public:
    template<class Service>
    static std::optional<SubmittedVerifierOwnership> bind(const VerifierSessionResources<Service>&,
        const SubmittedOwnershipObservation&,uint64_t now);
    const std::string& uuid()const{return uuid_;}
    const std::string& target()const{return target_;}
    const std::string& bundle()const{return bundle_;}
    const std::string& attempt()const{return attempt_;}
    uint64_t deadlineNS()const{return deadline_;}
    uint64_t observedNS()const{return observed_;}
    // A bounded session may reuse the startup relationship, but must independently
    // observe the same UUID and exact target on every sample. No fallback to an
    // absolute-path parser and no invented normalization of launchctl output.
    std::optional<VerifierJobSnapshot> snapshot(const JobProcessResult& job,
        const std::string& target,const std::string& program,const std::string& plist,
        uint64_t began,uint64_t ended,uint64_t now,uint64_t deadline)const{
        if(target!=target_ || program!=bundle_+"/Contents/MacOS/Pro40VerifierLab" ||
           plist!=bundle_+PRO40_LAB_PLIST_SUFFIX ||
           deadline!=deadline_ || !began || began<observed_ || ended<began || now<ended ||
           now>=deadline_ || now-began>=5000000000ULL ||
           !job.usable() || job.exit!=0 || !job.err.empty())return {};
        auto parsed=parseSubmittedVerifierJobSnapshot(job.out,target_);
        if(!parsed || parsed->btmUUID!=uuid_)return {};
        return parsed->state;
    }
};
}
