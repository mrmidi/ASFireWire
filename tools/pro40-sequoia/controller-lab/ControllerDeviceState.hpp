// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SessionDeviceState.hpp"
#include "JobObservation.hpp"
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include "SessionDeviceStatePins.hpp"

namespace Pro40Lab {
inline std::optional<SessionDeviceState> currentSessionDeviceState() {
    NSString *path=[NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Contents/MacOS/Pro40DeviceState"];
    SecStaticCodeRef code=nullptr;SecRequirementRef requirement=nullptr;
    auto status=SecRequirementCreateWithString(CFSTR(PRO40_DEVICE_STATE_REQUIREMENT),kSecCSDefaultFlags,&requirement);
    if(status==errSecSuccess)status=SecStaticCodeCreateWithPath((__bridge CFURLRef)[NSURL fileURLWithPath:path],kSecCSDefaultFlags,&code);
    if(status==errSecSuccess)status=SecStaticCodeCheckValidity(code,kSecCSStrictValidate,requirement);
    if(code)CFRelease(code);
    if(requirement)CFRelease(requirement);
    if(status!=errSecSuccess)return std::nullopt;
    const auto result=observeProcess({path.fileSystemRepresentation,"--current"},std::chrono::seconds(2),256);
    if(!result.usable() || result.exit!=0 || !result.err.empty())return std::nullopt;
    return parseSessionDeviceState(result.out,PRO40_MANAGED_REAL_GUID);
}
}
