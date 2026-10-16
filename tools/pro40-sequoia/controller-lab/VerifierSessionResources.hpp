// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "JournaledVerifierLaunch.hpp"
#include "NativeServiceOperations.hpp"
namespace Pro40Lab {
// The same object owns registration operations and subsequent launch/cleanup.
// bindRegistered freezes identity only after matching this journal's registered v5
// prefix; it neither registers nor imports a report as evidence of registration.
template<class Service>
class VerifierSessionResources final {
    NSDictionary *identity_=nil;
    NSString *attempt_=nil;
    LaunchNonce nonce_{};
    uint64_t deadline_=0;
    bool preparationClaimed_=false,ownershipCollectionClaimed_=false;
public:
    RegistrationJournal journal;
    ServiceOperations<Service> operations;
    std::unique_ptr<JournaledVerifierLaunch> launch;
    VerifierSessionResources(Service *service,dispatch_queue_t queue):operations(service,queue){}
    bool bindRegistered(NSString *attempt,NSDictionary *identity,LaunchNonce nonce,uint64_t deadline){
        if(launch || identity_)return false;
        auto candidate=JournaledVerifierLaunch::bind(journal,attempt,identity,nonce,deadline);
        if(!candidate)return false;
        NSData *data=[NSJSONSerialization dataWithJSONObject:identity options:0 error:nil];
        NSDictionary *snapshot=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
        if(!snapshot)return false;
        identity_=snapshot;attempt_=[attempt copy];nonce_=nonce;deadline_=deadline;launch=std::move(candidate);return true;
    }
    NSDictionary *identity()const{return identity_;}
    std::string registrationAttempt()const{return attempt_?attempt_.UTF8String:"";}
    bool registeredPrefixMatches()const{return attempt_ && identity_ && journal.matchesRegisteredLaunch(attempt_,identity_);}
    bool claimOwnershipCollection(){
        if(ownershipCollectionClaimed_ || preparationClaimed_ || !registeredPrefixMatches())return false;
        ownershipCollectionClaimed_=true;return true;
    }
    bool claimPreparation(){if(!launch || !identity_ || preparationClaimed_)return false;preparationClaimed_=true;return true;}
    LaunchNonce nonce()const{return nonce_;}
    uint64_t deadline()const{return deadline_;}
    std::string target()const{return identity_?[[NSString stringWithFormat:@"%@/%@",identity_[@"domain"],identity_[@"label"]] UTF8String]:"";}
    std::string program()const{return identity_?[[identity_[@"bundle_path"] stringByAppendingPathComponent:@"Contents/MacOS/Pro40VerifierLab"] UTF8String]:"";}
    std::string plist()const{return identity_?[[identity_[@"bundle_path"] stringByAppendingPathComponent:
        [@"Contents/Library/LaunchAgents" stringByAppendingPathComponent:identity_[@"plist"]]] UTF8String]:"";}
};
}
