// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "VerifierSessionResources.hpp"
#include "VerifierBTMRecords.hpp"
namespace Pro40Lab {
// BTM is a startup observation, never a per-audio/session-tick query.
struct SubmittedOwnershipObservation {
    JobProcessResult btm,job;
    uint64_t began=0,ended=0;
};
template<class Service>
std::optional<SubmittedVerifierOwnership> SubmittedVerifierOwnership::bind(
    const VerifierSessionResources<Service>& resources,
    const SubmittedOwnershipObservation& observation,uint64_t now){
    NSDictionary *identity=resources.identity();
    if(!resources.launch || !identity || !resources.registeredPrefixMatches() || !observation.began || observation.ended<observation.began ||
       now<observation.ended || now-observation.began>=10000000000ULL || now>=resources.deadline())return {};
    for(auto p:{&observation.btm,&observation.job})if(!p->usable() || p->exit!=0 || !p->err.empty())return {};
    auto job=parseSubmittedVerifierJobSnapshot(observation.job.out,resources.target());if(!job)return {};
    NSString *text=[[NSString alloc] initWithBytes:observation.btm.out.data() length:observation.btm.out.size() encoding:NSUTF8StringEncoding];
    NSArray *records=verifierBTMRecords(text,identity[@"uid"]);if(!records)return {};
    NSDictionary *parent=nil,*child=nil;
    NSString *parentID=[@"2." stringByAppendingString:identity[@"controller_id"]];
    NSString *childID=[@"8." stringByAppendingString:identity[@"label"]];
    for(NSDictionary *record in records){
        if([record[@"Identifier"] isEqual:parentID]){if(parent)return {};parent=record;}
        if([record[@"Identifier"] isEqual:childID]){if(child)return {};child=record;}
    }
    NSString *bundle=identity[@"bundle_path"];
    NSString *url=[NSURL fileURLWithPath:bundle isDirectory:YES].absoluteString;
    if(!parent || !child || ![parent[@"Type"] isEqual:@"app (0x2)"] || ![parent[@"URL"] isEqual:url] ||
       ![parent[@"Bundle Identifier"] isEqual:identity[@"controller_id"]] || ![child[@"Type"] isEqual:@"agent (0x8)"] ||
       ![child[@"Parent Identifier"] isEqual:parentID] ||
       ![child[@"UUID"] isEqual:[NSString stringWithUTF8String:job->btmUUID.c_str()]] ||
       ![child[@"URL"] isEqual:[@"Contents/Library/LaunchAgents/" stringByAppendingString:identity[@"plist"]]] ||
       ![child[@"Executable Path"] isEqual:@"Contents/MacOS/Pro40VerifierLab"])return {};
    return SubmittedVerifierOwnership(job->btmUUID,resources.target(),bundle.UTF8String,observation.ended,resources.deadline(),resources.registrationAttempt());
}
}
