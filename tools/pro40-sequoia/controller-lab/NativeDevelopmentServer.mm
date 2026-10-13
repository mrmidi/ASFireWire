#include "LabIdentity.hpp"
// SPDX-License-Identifier: Apache-2.0
#define OS_OBJECT_USE_OBJC 0
#import <os/log.h>
#include "BlockingVerifierServer.hpp"
#include "NativeDevelopmentIdentity.hpp"
#include "DevelopmentOriginalPeerPolicy.hpp"
#include "DevelopmentServerCommand.hpp"
namespace Pro40Lab {
int developmentServerCommand() {
    // Validate running helper identity before creating any listener. The caller
    // requirement is derived from fixed sibling provisioning, never CLI input.
    NSString *requirement=developmentPeerRequirement(DevelopmentRole::helper);
    BlockingVerifierServerResult result;
    if(requirement)result=runBlockingVerifierServer(nullptr,requirement.UTF8String,300000000);
    auto state=&result;
    NSDictionary *report=@{@"mode":@"development-listener-check",@"self_validated":@(requirement!=nil),
        @"prepared":@(state->prepared),@"local_cleanup_complete":@(state->completed),@"exchange_succeeded":@(state->success),
        @"reason":[NSString stringWithUTF8String:state->reason.c_str()],@"peer_authenticated":@NO,
        @"named_service_contacted":@NO,@"service_mutation_attempted":@NO,@"audio_admitted":@NO};
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nullptr];
    if(!json || fwrite(json.bytes,1,json.length,stdout)!=json.length || fputc('\n',stdout)==EOF)return 74;
    if(!requirement)return 2;
    return state->prepared && state->completed && !state->success && state->reason=="deadline-expired"?0:3;
}
int developmentServeVerifierCommand(){
    NSString *requirement=developmentPeerRequirement(DevelopmentRole::helper);
    if(!requirement){
        os_log_error(OS_LOG_DEFAULT,"[Pro40VerifierExit] self-provisioning-failed");
        return 2; // no listener before validated self and peer pins
    }
    auto identity=developmentOriginalIdentity(true);
    if(developmentOriginalPeerEnabled && !identity){
        os_log_error(OS_LOG_DEFAULT,"[Pro40VerifierExit] original-peer-policy-invalid");return 2;
    }
    const auto result=runBlockingVerifierServer(PRO40_LAB_SERVICE_ID,requirement.UTF8String,5000000000,std::move(identity));
    const bool success=result.prepared && result.completed && result.success;
    // One non-real-time terminal diagnostic; no pins, payloads or audio in log.
    if(!success)os_log_error(OS_LOG_DEFAULT,
        "[Pro40VerifierExit] prepared=%{public}d completed=%{public}d reason=%{public}s",
        int(result.prepared),int(result.completed),result.reason.c_str());
    return success?0:3;
}
}
