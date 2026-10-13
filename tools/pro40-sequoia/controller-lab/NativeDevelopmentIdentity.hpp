// SPDX-License-Identifier: Apache-2.0
#pragma once
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <mach-o/dyld.h>
#include <vector>
#include "DevelopmentPeerPins.hpp"
#include "DevelopmentOriginalPeerPolicy.hpp"

namespace Pro40Lab {
enum class DevelopmentRole { controller, helper };
// No caller-supplied path or hash: derive the fixed sibling location from this
// executable, then check the running code, not merely a file at that location.
inline NSString *developmentPeerRequirement(DevelopmentRole role) {
    uint32_t size=0;
    if(_NSGetExecutablePath(nullptr,&size)!=-1 || !size || size>65536)return nil;
    std::vector<char> path(size);
    if(_NSGetExecutablePath(path.data(),&size)!=0)return nil;
    std::string executable(path.data());
    const std::string suffix=std::string("/Pro40 Controller Lab.app/Contents/MacOS/")+
        (role==DevelopmentRole::controller?"Pro40ControllerLab":"Pro40VerifierLab");
    if(!executable.ends_with(suffix))return nil;
    DevelopmentPeerPins pins;
    if(!readDevelopmentPeerPins(executable.substr(0,executable.size()-suffix.size()),pins))return nil;
    const auto &own=role==DevelopmentRole::controller?pins.controller:pins.helper;
    const auto &peer=role==DevelopmentRole::controller?pins.helper:pins.controller;
    NSString *ownRequirement=[NSString stringWithFormat:@"cdhash H\"%s\"",own.c_str()];
    SecRequirementRef requirement=nullptr;
    SecCodeRef self=nullptr;
    // macOS SDK SecCode.h:51-63,214-238: CopySelf identifies this process;
    // CheckValidity checks its dynamic identity against the supplied requirement.
    OSStatus status=SecRequirementCreateWithString((__bridge CFStringRef)ownRequirement,
        kSecCSDefaultFlags,&requirement);
    if(status==errSecSuccess)status=SecCodeCopySelf(kSecCSDefaultFlags,&self);
    if(status==errSecSuccess)status=SecCodeCheckValidity(self,kSecCSDefaultFlags,requirement);
    if(self)CFRelease(self);
    if(requirement)CFRelease(requirement);
    if(status!=errSecSuccess)return nil;
    return [NSString stringWithFormat:@"cdhash H\"%s\"",peer.c_str()];
}
inline int developmentIdentityCommand(DevelopmentRole role) {
    NSString *requirement=developmentPeerRequirement(role);
    NSDictionary *report=@{@"mode":@"development-identity-check",
        @"role":role==DevelopmentRole::controller?@"controller":@"helper",
        @"self_validated":@(requirement!=nil),@"peer_requirement":requirement?:@"",
        @"peer_authenticated":@NO,@"service_mutation_attempted":@NO,@"audio_admitted":@NO};
    NSData *json=[NSJSONSerialization dataWithJSONObject:report
        options:NSJSONWritingSortedKeys error:nullptr];
    if(!json || fwrite(json.bytes,1,json.length,stdout)!=json.length || fputc('\n',stdout)==EOF)return 74;
    return requirement?0:2;
}
inline int developmentOriginalPolicyCommand(DevelopmentRole role){
    const bool selfValid=developmentPeerRequirement(role)!=nil;
    bool passed=selfValid;
    unsigned checks=0;
    if(selfValid && developmentOriginalPeerEnabled){
        Pro40Audio::NativeIdentityRequest request;request.role=1;request.guid=42;request.epoch=7;
        request.requestID[0]=1;request.policyHash=developmentOriginalPolicyHash();request.token.val[0]=3;
        auto helper=developmentOriginalIdentity(true);
        auto controller=developmentOriginalIdentity(false,request);
        passed=helper && helper->check && helper->policy==request.policyHash && !helper->request &&
            controller && controller->request==request && !controller->check;checks+=2;
        passed=passed && !developmentOriginalIdentity(false) && !developmentOriginalIdentity(true,request);checks+=2;
        request.policyHash[0]^=1;
        passed=passed && !developmentOriginalIdentity(false,request);++checks;
        const audit_token_t invalid=INVALID_AUDIT_TOKEN_VALUE;
        passed=passed && helper && helper->check(invalid)==errSecParam;++checks;
    }
    NSDictionary *report=@{@"mode":@"original-peer-policy-check",@"self_validated":@(selfValid),
        @"enabled":@(developmentOriginalPeerEnabled),@"checks":@(checks),@"passed":@(passed),
        @"original_peer_authenticated":@NO,@"service_mutation_attempted":@NO,@"audio_admitted":@NO};
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nullptr];
    if(!json || fwrite(json.bytes,1,json.length,stdout)!=json.length || fputc('\n',stdout)==EOF)return 74;
    return passed?0:2;
}
}
