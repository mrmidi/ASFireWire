// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RegistrationJournal.hpp"
#include "JobObservation.hpp"
namespace Pro40Lab {
enum class LaunchRecoveryReviewResult { unavailable, currentBoot, invalidObservation, eligibleAfterReboot };
// Read-only eligibility, never recovery permission or completion. The caller
// supplies fresh trusted native observations while retaining the journal lock.
inline LaunchRecoveryReviewResult reviewLaunchRecovery(RegistrationJournal& journal,
    NSString *attempt,NSString *hash,NSDictionary *observation,const JobProcessResult& helper){
    NSDictionary *record=journal.reviewLaunchFailure(attempt,hash);
    if(!record)return LaunchRecoveryReviewResult::unavailable;
    NSDictionary *current=observation[@"identity"],*old=record[@"metadata"];
    if(!RegistrationJournal::validatePreflight(current,observation) ||
       ![observation[@"experiment"] isEqual:@"ordinary"] || [observation[@"raw_status"] integerValue]!=0)
        return LaunchRecoveryReviewResult::invalidObservation;
    // Package path, both code hashes, plist hash and user/domain all stay fixed.
    for(NSString *key in old){
        if([@[@"pid",@"boot",@"start_seconds",@"start_microseconds"] containsObject:key])continue;
        if(![old[key] isEqual:current[key]])return LaunchRecoveryReviewResult::invalidObservation;
    }
    if([old[@"boot"] caseInsensitiveCompare:current[@"boot"]]==NSOrderedSame)
        return LaunchRecoveryReviewResult::currentBoot;
    // Fixed pgrep -x Pro40VerifierLab is collected by the future native review
    // owner. Only explicit no-match is absence, not a timeout/error/empty success.
    if(!helper.usable() || !helper.reaped || helper.exit!=1 || !helper.out.empty() || !helper.err.empty())
        return LaunchRecoveryReviewResult::invalidObservation;
    // Reboot discharges old process/IPC lifetime, not registration. Fresh status
    // and exact launchctl absence above independently corroborate removal.
    return LaunchRecoveryReviewResult::eligibleAfterReboot;
}
}
