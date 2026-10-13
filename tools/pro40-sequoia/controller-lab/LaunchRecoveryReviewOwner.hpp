// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "LaunchRecoveryReview.hpp"
namespace Pro40Lab {
struct LaunchRecoveryObservation {NSDictionary *candidate=nil;JobProcessResult helper;};
enum class LaunchRecoveryOwnerResult { refused, unavailable, stopped, expired, currentBoot, invalidObservation, eligibleAfterReboot, recovered, persistenceFailed };
class LaunchRecoveryReviewOwner final {
    RegistrationJournal journal_;
    bool used_=false;
    bool writeAttempted_=false;
public:
    using Clock=std::chrono::steady_clock;
    const std::string& error()const{return journal_.error;}
    bool writeAttempted()const{return writeAttempted_;}
#ifdef PRO40_JOURNAL_TESTING
    void failWriteAt(int stage){journal_.failWriteStage=stage;}
#endif
private:
    template<class Collect,class Now,class Stop>
    LaunchRecoveryOwnerResult inspect(const std::string& path,NSString *attempt,NSString *hash,
        Collect collect,Now now,Stop stop,bool execute){
        if(used_)return LaunchRecoveryOwnerResult::refused;
        used_=true;const auto start=now();auto last=start;
        auto check=[&]{
            if(stop())return LaunchRecoveryOwnerResult::stopped;
            const auto time=now();const bool expired=time<last || time-start>=std::chrono::seconds(5);
            last=time;return expired?LaunchRecoveryOwnerResult::expired:LaunchRecoveryOwnerResult::eligibleAfterReboot;
        };
        auto permission=check();if(permission!=LaunchRecoveryOwnerResult::eligibleAfterReboot)return permission;
        if(!journal_.open(path,!execute,true))return LaunchRecoveryOwnerResult::unavailable;
        if(!journal_.reviewLaunchFailure(attempt,hash))return LaunchRecoveryOwnerResult::unavailable;
        permission=check();if(permission!=LaunchRecoveryOwnerResult::eligibleAfterReboot)return permission;
        auto observed=collect();
        permission=check();if(permission!=LaunchRecoveryOwnerResult::eligibleAfterReboot)return permission;
        if(![observed.candidate isKindOfClass:[NSDictionary class]] ||
           !RegistrationJournal::validatePreflight(observed.candidate[@"identity"],observed.candidate))
            return LaunchRecoveryOwnerResult::invalidObservation;
        NSMutableDictionary *candidate=[observed.candidate mutableCopy];
        candidate[@"elapsed_ns"]=@(std::chrono::duration_cast<std::chrono::nanoseconds>(last-start).count());
        const auto result=reviewLaunchRecovery(journal_,attempt,hash,candidate,observed.helper);
        permission=check();if(permission!=LaunchRecoveryOwnerResult::eligibleAfterReboot)return permission;
        switch(result){
            case LaunchRecoveryReviewResult::eligibleAfterReboot:break;
            case LaunchRecoveryReviewResult::currentBoot:return LaunchRecoveryOwnerResult::currentBoot;
            case LaunchRecoveryReviewResult::invalidObservation:return LaunchRecoveryOwnerResult::invalidObservation;
            default:return LaunchRecoveryOwnerResult::unavailable;
        }
        if(!execute)return LaunchRecoveryOwnerResult::eligibleAfterReboot;
        candidate[@"elapsed_ns"]=@(std::chrono::duration_cast<std::chrono::nanoseconds>(last-start).count());
        NSDictionary *evidence=@{@"record_sha256":hash,@"observation":candidate,
            @"helper_absence":@{@"argv":@[@"/usr/bin/pgrep",@"-x",@"Pro40VerifierLab"],
                @"exit":@(observed.helper.exit),@"stdout":@"",@"stderr":@"",@"usable":@YES,@"reaped":@YES},
            @"review_reference":@"post-reboot-v1: locked exact record; fresh matching package and registration/job/helper absence"};
        permission=check();if(permission!=LaunchRecoveryOwnerResult::eligibleAfterReboot)return permission;
        writeAttempted_=true;
        // A late fsync cannot turn a stored result into a refusal. Failure may
        // occur after rename; callers must re-inspect rather than assume no write.
        return journal_.recoverLaunchFailure(attempt,hash,evidence)?LaunchRecoveryOwnerResult::recovered:
            LaunchRecoveryOwnerResult::persistenceFailed;
    }
public:
    template<class Collect,class Now,class Stop>
    LaunchRecoveryOwnerResult review(const std::string& path,NSString *attempt,NSString *hash,Collect collect,Now now,Stop stop){
        return inspect(path,attempt,hash,collect,now,stop,false);
    }
    template<class Collect,class Now,class Stop>
    LaunchRecoveryOwnerResult recover(const std::string& path,NSString *attempt,NSString *hash,Collect collect,Now now,Stop stop){
        return inspect(path,attempt,hash,collect,now,stop,true);
    }
};
}
