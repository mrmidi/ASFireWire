#include "LabIdentity.hpp"
// SPDX-License-Identifier: Apache-2.0
#pragma once
#import <Foundation/Foundation.h>
namespace Pro40Lab {
enum class VerifierServiceProfile { dormant, onDemand };
inline NSDictionary *verifierServiceDefinition(VerifierServiceProfile profile){
    NSMutableDictionary *definition=[@{@"Label":@PRO40_LAB_SERVICE_ID,
        @"BundleProgram":@"Contents/MacOS/Pro40VerifierLab",@"RunAtLoad":@NO,@"KeepAlive":@NO} mutableCopy];
    if(profile==VerifierServiceProfile::onDemand){
        definition[@"ProgramArguments"]=@[@"Pro40VerifierLab",@"--serve-verifier"];
        definition[@"MachServices"]=@{@PRO40_LAB_SERVICE_ID:@YES};
    }
    return [definition copy];
}
inline bool verifierServiceDefinitionMatches(id candidate,VerifierServiceProfile profile){
    if(![candidate isKindOfClass:NSDictionary.class] || ![candidate isEqual:verifierServiceDefinition(profile)])return false;
    // NSDictionary numeric equality alone accepts integers in place of plist booleans.
    for(NSString *key in @[@"RunAtLoad",@"KeepAlive"])
        if(CFGetTypeID((__bridge CFTypeRef)candidate[key])!=CFBooleanGetTypeID())return false;
    if(profile==VerifierServiceProfile::onDemand && CFGetTypeID((__bridge CFTypeRef)
        candidate[@"MachServices"][@PRO40_LAB_SERVICE_ID])!=CFBooleanGetTypeID())return false;
    return true;
}
}
