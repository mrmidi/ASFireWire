#include "LabIdentity.hpp"
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "VerifierSessionResources.hpp"
#include "ControllerSessionBinding.hpp"
#include "NativeRegistrationCollector.hpp"
#include "VerifierRegistrationPreparation.hpp"
#include "SubmittedOwnershipPreparation.hpp"
namespace Pro40Lab {
using NativeVerifierResources=VerifierSessionResources<SMAppService>;
inline std::shared_ptr<NativeVerifierResources> makeNativeVerifierResources(dispatch_queue_t queue){
    dispatch_assert_queue(queue);
    auto service=[SMAppService agentServiceWithPlistName:@PRO40_LAB_PLIST];
    return std::make_shared<NativeVerifierResources>(service,queue);
}
// Called only after the same resources registered and bound their v5 journal.
// Preparation is inactive; this helper does not call session.start or authorize it.
inline ControllerSessionBinding<NativeVerifierResources> prepareNativeVerifierSession(
    dispatch_queue_t queue,std::shared_ptr<NativeVerifierResources> resources,
    std::function<void(NSDictionary *)> report,
    std::optional<Pro40Audio::NativeIdentityRequest> originalRequest={}){
    dispatch_assert_queue(queue);
    if(!resources || !report || !resources->launch || !resources->launch->hasSubmittedOwnership() || !resources->claimPreparation())return {};
    const auto target=resources->target();NSDictionary *identity=resources->identity();
    return bindControllerSession<NativeVerifierResources>(queue,resources,resources->nonce(),resources->deadline(),
        []{return observationNS();},[target](int32_t pid){return collectVerifierObservation(target,pid);},
        [identity]{
            auto removal=collectNativeVerifierRemovalReport(identity);
            return [removal[@"valid"] boolValue]?removal[@"removal_evidence"]:nil;
        },std::move(report),[queue,originalRequest](LaunchNonce n,uint64_t d,NativeVerifierConnection::Callbacks callbacks){
            return prepareControllerConnection((__bridge void *)queue,n,d,std::move(callbacks),originalRequest);
        },originalRequest);
}
struct NativeVerifierStartup {
    VerifierRegistrationResult registration=VerifierRegistrationResult::refused;
    ControllerSessionBinding<NativeVerifierResources> binding;
};
// Caller retains resources and returned binding. A returned session is inactive:
// start it even on preparation refusal to drain cleanup, per binding contract.
// Command exposure remains controlled by PRO40_ENABLE_VERIFIER_EXPERIMENT.
inline NativeVerifierStartup registerAndPrepareNativeVerifierSession(dispatch_queue_t queue,
    std::shared_ptr<NativeVerifierResources> resources,const std::string& path,NSString *attempt,
    LaunchNonce nonce,uint64_t deadline,std::function<bool()> stop,
    std::function<void(NSDictionary *)> report,
    std::optional<Pro40Audio::NativeIdentityRequest> originalRequest={}){
    dispatch_assert_queue(queue);
    if(!resources || !stop || !report)return {};
    if(originalRequest && (!originalRequest->valid() || originalRequest->requestID!=nonce))return {};
    VerifierRegistrationPreparation<SMAppService> startup(queue,resources);
    auto result=startup.run(path,attempt,nonce,deadline,[]{
        NSDictionary *candidate=collectNativeVerifierRegistrationReport()[@"intent_candidate"];
        return CollectedRegistrationEvidence{candidate[@"identity"],candidate};
    },[]{return observationNS();},stop);
    if(result!=VerifierRegistrationResult::bound)return {result,{}};
    const bool stopped=stop();
    if(stopped || observationNS()>=deadline){
        resources->launch->abandon(stopped?@"cancelled":@"expired",@"startup stopped before connection preparation",observationNS());
        return {VerifierRegistrationResult::blocked,{}};
    }
    if(!prepareSubmittedOwnership(*resources,[&]{
        return collectNativeSubmittedOwnership(resources->target(),deadline,stop);
    },[]{return observationNS();},stop))return {VerifierRegistrationResult::blocked,{}};
    // Recheck after proof installation, before allocating a connection.
    const bool cancelled=stop();
    if(cancelled || observationNS()>=deadline){
        resources->launch->abandon(cancelled?@"cancelled":@"expired",@"startup stopped after ownership binding",observationNS());
        return {VerifierRegistrationResult::blocked,{}};
    }
    auto binding=prepareNativeVerifierSession(queue,resources,std::move(report),originalRequest);
    if(!binding.session){
        resources->launch->abandon(@"api-error",@"session preparation unavailable",observationNS());
        return {VerifierRegistrationResult::blocked,{}};
    }
    return {result,std::move(binding)};
}
}
