// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "VerifierSessionResources.hpp"
#include "RegistrationPreparation.hpp"
namespace Pro40Lab {
enum class VerifierRegistrationResult { refused, unavailable, invalidEvidence, blocked, bound };
// Serial startup stage. The same retained resources own registration, launch,
// and later cleanup. A bound result permits session preparation, not activation.
// Failure after durable intent leaves an unresolved record; never infer removal.
template<class Service>
class VerifierRegistrationPreparation final {
    dispatch_queue_t queue_;
    std::shared_ptr<VerifierSessionResources<Service>> resources_;
    bool consumed_=false;
public:
    VerifierRegistrationPreparation(dispatch_queue_t q,std::shared_ptr<VerifierSessionResources<Service>> r):
        queue_(q),resources_(std::move(r)){}
    template<class Collector,class Now,class Stop>
    VerifierRegistrationResult run(const std::string& path,NSString *attempt,LaunchNonce nonce,uint64_t deadline,
        Collector collect,Now now,Stop stop){
        dispatch_assert_queue(queue_);
        if(consumed_ || !resources_)return VerifierRegistrationResult::refused;
        consumed_=true;
        bool any=false;for(auto byte:nonce)any|=byte!=0;
        if(!any || !deadline)return VerifierRegistrationResult::refused;
        const auto began=now();auto last=began;
        auto permit=[&]{
            const auto t=now();
            if(stop())return ActionPermission::cancelled;
            if(t<last)return ActionPermission::invalidObservation;
            last=t;
            return t>=deadline || t-began>=5000000000?ActionPermission::expired:ActionPermission::allowed;
        };
        auto &j=resources_->journal;
        if(permit()!=ActionPermission::allowed)return VerifierRegistrationResult::blocked;
        if(!j.open(path) || !j.requireClear())return VerifierRegistrationResult::unavailable;
        if(permit()!=ActionPermission::allowed)return VerifierRegistrationResult::blocked;
        auto evidence=collect(); // fresh, under journal lock
        if(permit()!=ActionPermission::allowed)return VerifierRegistrationResult::blocked;
        if(!RegistrationJournal::validatePreflight(evidence.identity,evidence.preflight))return VerifierRegistrationResult::invalidEvidence;
        NSData *bytes=[NSJSONSerialization dataWithJSONObject:@{@"identity":evidence.identity,@"preflight":evidence.preflight} options:0 error:nil];
        NSDictionary *copy=bytes?[NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil]:nil;
        if(!copy)return VerifierRegistrationResult::invalidEvidence;
        if(permit()!=ActionPermission::allowed)return VerifierRegistrationResult::blocked;
        NSMutableDictionary *preflight=[copy[@"preflight"] mutableCopy];preflight[@"elapsed_ns"]=@(last-began);
        if(!j.beginLaunch(attempt,copy[@"identity"],preflight))return VerifierRegistrationResult::blocked;
        auto fail=[&](ActionPermission p,NSString *detail){
            NSString *category=p==ActionPermission::cancelled?@"cancelled":p==ActionPermission::expired?@"expired":@"observation-invalid";
            (void)j.recordLaunchFailure(category,detail,last);
            return VerifierRegistrationResult::blocked;
        };
        auto permission=permit();if(permission!=ActionPermission::allowed)return fail(permission,@"registration denied after intent");
        NSDictionary *result=resources_->operations.registerOnce([&]{permission=permit();return permission==ActionPermission::allowed;});
        if(!result)return fail(permission,@"registration refused or result unrepresentable");
        // Preserve the API result even if the synchronous call exhausted time.
        if(!j.append(@"register_result",result))return VerifierRegistrationResult::blocked;
        permission=permit();if(permission!=ActionPermission::allowed)return fail(permission,@"registration returned outside permission");
        if(!resources_->bindRegistered(attempt,copy[@"identity"],nonce,deadline)){
            (void)j.recordLaunchFailure(@"api-error",@"registration did not establish an owned enabled service",last);
            return VerifierRegistrationResult::blocked;
        }
        return VerifierRegistrationResult::bound;
    }
};
}
