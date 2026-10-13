// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeVerifierSession.hpp"
#include "VerifierCommandSupervisor.hpp"
#include "ExperimentCommand.hpp"
#include "TestCapturedIdentityPeer.hpp"
#include "DevelopmentOriginalPeerPolicy.hpp"
#include "VerifierWorkerEntry.hpp"
#include "../NativeVerifiedSourceConfiguration.hpp"
#include "NativeWorkerDiagnosticCommand.hpp"
namespace Pro40Lab {
inline ExperimentControl makeNativeVerifierCommand(dispatch_queue_t queue,NSString *path,NSString *attempt,
    std::function<void(NSDictionary *)> report,std::string originalSender={},bool stopBeforeDecision=false){
#ifndef PRO40_ENABLE_ORIGINAL_PEER_DIAGNOSTIC
    if(stopBeforeDecision)return {}; // Never enable diagnostic injection in ordinary builds.
#endif
    using Supervisor=VerifierCommandSupervisor<NativeVerifierResources>;
    struct State {std::atomic<bool> stop{false};std::shared_ptr<Supervisor> owner;std::unique_ptr<TestCapturedIdentityPeer> peer;
        std::optional<Pro40Audio::NativeVerifiedSourceConfiguration> configuration;};
    auto state=std::make_shared<State>();
    std::weak_ptr<State> weakState=state;
    auto requestStop=[weakState,queue]{
        auto state=weakState.lock();if(!state)return;
        state->stop.store(true);
        dispatch_async(queue,^{if(state->owner)state->owner->requestStop();});
    };
    return {
        [state,queue,path,attempt,report,originalSender,stopBeforeDecision,requestStop]{
            dispatch_assert_queue(queue);
            auto resources=makeNativeVerifierResources(queue);
            std::weak_ptr<State> weak=state;
            auto resultReport=[weak,report,stopBeforeDecision,requestStop](NSDictionary *value){
                auto state=weak.lock();if(!state)return;
                if(!state->peer || ![value[@"supervision_finished"] boolValue]){report(value);return;}
                // Evaluate while the owned peer is alive. This is diagnostic
                // evidence only: no runner or device lease is entered.
                if(stopBeforeDecision)requestStop();
                auto outcome=state->owner->identityOutcome(state->stop.load());
                std::optional<int32_t> status;bool permits=false;
                if(outcome){Pro40Audio::NativeServiceIdentityDecision decision(*outcome);
                    status=decision.boundStatus();permits=decision.permitsAdmission();}
                NSMutableDictionary *final=[value mutableCopy];
                final[@"original_peer_pid"]=@(state->peer->pid());
                final[@"original_peer_status"]=status?@(int32_t(*status)):[NSNull null];
                final[@"identity_reply_status"]=outcome && outcome->reply?@(int32_t(outcome->reply->status)):[NSNull null];
                final[@"command_stop_observed"]=@(state->stop.load());
                final[@"diagnostic_stop_before_decision"]=@(stopBeforeDecision);
                final[@"identity_decision_permits_admission"]=@(permits);
                final[@"decision_evaluated_before_peer_reap"]=@YES;
#ifdef PRO40_ENABLE_ORIGINAL_PEER_DIAGNOSTIC
                std::unique_ptr<VerifierWorkerEntry<Supervisor>> retainedEntry;
                if(state->configuration){
                    // This native in-process path never creates a receipt pipe.
                    retainedEntry=std::make_unique<VerifierWorkerEntry<Supervisor>>(state->owner,state->configuration->request(),
                        Pro40Audio::ControllerReceiptObligation::none);
                    auto &entry=*retainedEntry;
                    Pro40::StopQuery stop{[](void *p)noexcept{return static_cast<State *>(p)->stop.load();},state.get()};
                    unsigned calls=0;
                    auto count=[&](Pro40::StopQuery){++calls;return Pro40::NativeSessionResult{0,{}};};
                    const auto entered=entry.enter(stop,count);
                    const auto replay=entry.enter(stop,count);
                    final[@"diagnostic_worker_entries"]=@(calls);
                    final[@"diagnostic_gate_entered"]=@(entered.status==Pro40Audio::ServiceEntryStatus::entered);
                    final[@"diagnostic_replay_refused"]=@(replay.status==Pro40Audio::ServiceEntryStatus::consumed);
                    auto before=entry.snapshot();
                    final[@"diagnostic_cleanup_before_reap"]=@(before && before->cleanupComplete());
                }
#endif
                state->peer->reap();final[@"original_peer_reaped"]=@YES;
#ifdef PRO40_ENABLE_ORIGINAL_PEER_DIAGNOSTIC
                if(retainedEntry){
                    // reap returns only after close and successful waitpid; capture
                    // Mach rights were already released by the peer constructor.
                    const bool reaped=retainedEntry->confirmSenderReaped();
                    const bool closed=retainedEntry->confirmSenderEndpointsClosed();
                    auto result=retainedEntry->snapshot();
                    final[@"diagnostic_controller_cleanup_confirmed"]=@(reaped && closed && result && result->controllerCleanup()==0);
                    final[@"diagnostic_attempt_valid"]=@(result && result->valid());
                    final[@"diagnostic_attempt_cleanup_complete"]=@(result && result->cleanupComplete());
                    final[@"diagnostic_attempt_accepted"]=@(result && result->accepted());
                }
#endif
                final[@"audio_admitted"]=@NO;report(final);
            };
            state->owner=Supervisor::create(queue,resources,resultReport,[]{return observationNS();});
            LaunchNonce nonce{};arc4random_buf(nonce.data(),nonce.size());
            const auto deadline=observationNS()+nativeVerifierDeadlineNS;
            std::optional<Pro40Audio::NativeIdentityRequest> originalRequest;
            if(!originalSender.empty()){
                state->peer=std::make_unique<TestCapturedIdentityPeer>(originalSender.c_str());
                // Explicit diagnostic settings, never inferred from a helper reply.
                Pro40::NativeSessionOptions diagnosticOptions;diagnosticOptions.guid=42;
                state->configuration=Pro40Audio::NativeVerifiedSourceConfiguration::create(
                    std::move(diagnosticOptions),7,nonce,developmentOriginalPolicyHash(),state->peer->token());
                if(!state->configuration)std::_Exit(80); // Do not downgrade to helper-only verification.
                originalRequest=state->configuration->request();
            }
            if(state->stop.load())state->owner->requestStop();
            (void)state->owner->start([&](Supervisor::Report progress,std::function<bool()> stopped){
                auto result=registerAndPrepareNativeVerifierSession(queue,resources,path.fileSystemRepresentation,
                    attempt,nonce,deadline,[&]{return state->stop.load()||stopped();},std::move(progress),originalRequest);
                return std::move(result.binding);
            },[resources]{return collectVerifierObservation(resources->target());});
        },
        std::move(requestStop)
    };
}
}
