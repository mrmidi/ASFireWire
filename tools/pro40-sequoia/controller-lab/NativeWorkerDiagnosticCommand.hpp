// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeVerifierSession.hpp"
#include "VerifierCommandSupervisor.hpp"
#include "ExperimentCommand.hpp"
#include "DevelopmentOriginalPeerPolicy.hpp"
#include "VerifierStartupEntry.hpp"
#include "WorkerCommandCompletion.hpp"
#include <vector>
#include <type_traits>
#include "../worker/WorkerDiagnosticTiming.hpp"
#include "../worker/WorkerLaunchSpecification.hpp"
namespace Pro40Lab {
// Locally chosen launch values, never supplied by an identity helper reply.
// Executable identity is pinned by the package. Hardware-free diagnostics and
// the separately gated finite real-recording profile share the managed lifecycle.
struct NativeWorkerDiagnosticLaunch {
    std::string executable;
    std::vector<std::string> environment;
    std::optional<Pro40::WorkerLaunchSpecification> specification;
    uint64_t epoch=0;
    int errorFD=-1; // borrowed through command start
    std::shared_ptr<WorkerDuplexConnectionOwner> duplex;
    Pro40::ManagedRunTiming::Lifetime lifetime=Pro40::ManagedRunTiming::Lifetime::bounded;
#ifdef PRO40_TIMING_TESTING
    bool failTimingTimer=false;
#endif
};
template<bool Socket, bool Timed=false> inline ExperimentControl makeWorkerDiagnosticCommandImpl(dispatch_queue_t queue,NSString *path,NSString *attempt,
    std::function<void(NSDictionary *)> report,NativeWorkerDiagnosticLaunch launch,bool stopBeforeDecision=false){
#if !defined(PRO40_ENABLE_GATED_WORKER_DIAGNOSTIC) || !defined(PRO40_ENABLE_ORIGINAL_PEER_DIAGNOSTIC)
    (void)queue;(void)path;(void)attempt;(void)report;(void)launch;(void)stopBeforeDecision;return {};
#else
#ifndef PRO40_ENABLE_MANAGED_SOCKET_DIAGNOSTIC
    if constexpr(Socket)return {};
#endif
    static_assert(!Timed || Socket);
#ifndef PRO40_ENABLE_MANAGED_TIMED_DIAGNOSTIC
    if constexpr(Timed)return {};
#endif
    if(!launch.specification)return {};
    if(launch.lifetime==Pro40::ManagedRunTiming::Lifetime::untilStopped && (!Timed || !launch.duplex))return {};
    if(bool(launch.duplex)!=bool(launch.specification->duplex()) ||
       (launch.duplex && (!Socket || !Timed || launch.specification->duplex()->epoch!=launch.epoch)))return {};
    static_assert(Pro40::WorkerDiagnosticTiming::capture+std::chrono::nanoseconds(nativeVerifierDeadlineNS)
                  <Pro40::WorkerDiagnosticTiming::admission);
    using Supervisor=VerifierCommandSupervisor<NativeVerifierResources>;
    using CaptureOwner=std::conditional_t<Socket,WorkerSocketCaptureOwner,WorkerCaptureOwner>;
    using CaptureReport=std::conditional_t<Socket,WorkerSocketCaptureReport,WorkerCaptureReport>;
    using Completion=BasicWorkerCommandCompletion<CaptureReport,Timed>;
    struct State {
        std::atomic<bool> stop{false};
        bool started=false,verifierFinished=false,entryResolved=false,released=false;
        std::shared_ptr<Supervisor> verifier;
        std::shared_ptr<WorkerStartupOwner> startup;
        std::shared_ptr<ManagedRunOwner> timing;
        RecordingExportEvidence recording;
        std::shared_ptr<WorkerDuplexConnectionOwner> duplex;
        std::shared_ptr<CaptureOwner> capture;
        std::shared_ptr<Completion> completion;
        std::unique_ptr<VerifierStartupEntry<Supervisor>> entry;
        std::optional<Pro40Audio::NativeVerifiedSourceConfiguration> configuration;
        std::function<void()> maybeFinish;
    };
    auto state=std::make_shared<State>();std::weak_ptr<State> weak=state;
    state->duplex=launch.duplex;
    state->completion=std::make_shared<Completion>(queue,std::move(report),bool(state->duplex));
    state->maybeFinish=[weak]{
        auto s=weak.lock();if(!s || s->entryResolved || !s->verifierFinished || !s->startup || !s->capture)return;
        const auto worker=s->startup->snapshot();const auto capture=s->capture->snapshot();
        if(!worker.finished || !worker.resourcesComplete || !capture.endpointsClosed)return;
        if(s->duplex && !s->duplex->snapshot().finished)return;
        std::optional<Pro40Audio::NativeServiceAttemptResult> result;
        if(s->entry){
            if(s->entry->finish(true)){auto outcome=s->entry->snapshot();if(outcome)result.emplace(*outcome);}
            else if(s->released)return; // Keep a released worker's unresolved entry owned.
        }
        s->entryResolved=s->completion->resolveEntry(result,s->recording.finish(result && result->accepted()));
    };
    auto stop=[weak,queue]{
        auto s=weak.lock();if(!s)return;s->stop.store(true);
        dispatch_async(queue,^{
            if(s->timing)s->timing->requestStop();
            if(s->startup)s->startup->requestStop();
            if(s->verifier)s->verifier->requestStop();
            if(s->capture)s->capture->cancel();
            if(s->duplex)s->duplex->requestStop();
            s->completion->observeStop();
        });
    };
    return {[state,weak,queue,path,attempt,launch=std::move(launch),stopBeforeDecision]{
        dispatch_assert_queue(queue);if(state->started)return;state->started=true;
        const auto began=std::chrono::steady_clock::now();
        LaunchNonce nonce{};arc4random_buf(nonce.data(),nonce.size());
        // No verifier registration/resources exist until successful capture.
        auto skipped=[weak](NSString *reason){if(auto s=weak.lock()){
            if(s->verifierFinished)return;s->verifierFinished=true;
            if(s->duplex){s->duplex->retireUnreleased();s->completion->duplexReport(s->duplex->snapshot());}
            s->completion->verifierReport(@{@"supervision_finished":@YES,@"local_cleanup_complete":@YES,
                @"registration_reconciled":@YES,@"verifier_not_started":@YES,@"verifier_not_started_reason":reason,@"helper_clean_exit_proven":@NO});
            s->startup->requestStop();s->maybeFinish();
        }};
        state->startup=WorkerStartupOwner::create(queue,[weak](WorkerStartupReport value){if(auto s=weak.lock()){
            s->completion->workerReport(value);
            if(value.finished && s->duplex)s->duplex->requestStop();
            if(s->timing)s->timing->pollAfterStartFailure();
            s->maybeFinish();
        }});
        if constexpr(Timed){
            state->timing=ManagedRunOwner::create(queue,state->startup,began,[weak](ManagedRunReport value){
                if(auto s=weak.lock())s->completion->timingReport(value);
            },[]{return Pro40::ManagedRunTiming::Clock::now();},launch.lifetime);
#ifdef PRO40_TIMING_TESTING
            if(launch.failTimingTimer)state->timing->failTimerForTesting();
#endif
            (void)state->timing->start();
        }
        state->capture=CaptureOwner::create(queue,[weak,queue,path,attempt,launch,nonce,skipped,stopBeforeDecision](CaptureReport value){
            auto s=weak.lock();if(!s)return;s->completion->captureReport(value);
            if(value.status!=CaptureReport::Status::captured || !value.capture || s->stop.load() || !s->startup->readyToVerify()){
                skipped(s->stop.load()?@"stopped-before-verification":@"capture-or-startup-unavailable");return;
            }
            s->configuration=Pro40Audio::NativeVerifiedSourceConfiguration::create(
                launch.specification->identityOptions(),launch.epoch,nonce,developmentOriginalPolicyHash(),value.capture->token());
            if(!s->configuration){skipped(@"invalid-source-configuration");return;}
            auto resources=makeNativeVerifierResources(queue);
            s->verifier=Supervisor::create(queue,resources,[weak,queue,stopBeforeDecision,launch](NSDictionary *report){
                auto owner=weak.lock();if(!owner)return;
                // Raw terminal report stays inside this command, never reaches the envelope.
                if(![report[@"supervision_finished"] boolValue]){owner->completion->verifierReport(report);return;}
                const auto evidence=owner->verifier->identityOutcome(owner->stop.load());
                NSMutableDictionary *terminal=[report mutableCopy];
                terminal[@"identity_reply_status"]=evidence && evidence->reply?@(evidence->reply->status):[NSNull null];
                terminal[@"diagnostic_stop_before_decision"]=@(stopBeforeDecision);
                if(stopBeforeDecision)owner->stop.store(true);
                owner->verifierFinished=true;
                if(owner->stop.load()){owner->startup->requestStop();owner->completion->observeStop();}
                if(owner->configuration && owner->startup->readyToVerify()){
                    owner->entry=std::make_unique<VerifierStartupEntry<Supervisor>>(owner->verifier,*owner->configuration,owner->startup);
                    auto release=[&]{
                        owner->released=owner->entry->begin(*owner->configuration)==VerifierStartupEntry<Supervisor>::ChildStart::released;
                        return owner->released;
                    };
                    if constexpr(Timed){
                        // Track actual release even when the post-release timing check fails.
                        // The entry still owns a child and must finish before retirement.
                        terminal[@"managed_release_timing_accepted"]=@(owner->timing->release(release));
                    }else (void)release();
                }
                terminal[@"diagnostic_worker_released"]=@(owner->released);
                if(owner->duplex) {
                    if(owner->released) {
                        const auto &d=*launch.specification->duplex();const auto request=owner->configuration->request();
                        const bool started=owner->duplex->start(request.token,{d.epoch,d.nonce,request.requestID},[weak]{
                            if(auto s=weak.lock()) {
                                s->completion->duplexReport(s->duplex->snapshot());
                                s->startup->requestStop();s->maybeFinish();
                            }
                        });
                        if(!started){owner->duplex->retireUnreleased();owner->completion->duplexReport(owner->duplex->snapshot());owner->startup->requestStop();}
                    }else{owner->duplex->retireUnreleased();owner->completion->duplexReport(owner->duplex->snapshot());}
                }
                owner->completion->verifierReport(terminal);
                if(owner->released && !Socket){
                    // Finite fake-runner diagnostic only; never an indefinite audio session.
                    auto startup=owner->startup;
                    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,250*NSEC_PER_MSEC),queue,^{startup->requestStop();});
                }else if(!owner->released)owner->startup->requestStop();
                owner->maybeFinish();
            },[]{return observationNS();});
            const auto request=s->configuration->request();const auto deadline=observationNS()+nativeVerifierDeadlineNS;
            if(s->stop.load())s->verifier->requestStop();
            (void)s->verifier->start([s,queue,resources,path,attempt,nonce,deadline,request](Supervisor::Report progress,std::function<bool()> stopped){
                auto result=registerAndPrepareNativeVerifierSession(queue,resources,path.fileSystemRepresentation,
                    attempt,nonce,deadline,[&]{return s->stop.load()||stopped();},std::move(progress),request);
                return std::move(result.binding);
            },[resources]{return collectVerifierObservation(resources->target());});
        });
        if(state->stop.load())state->startup->requestStop();
        if(!state->recording.prepare(*launch.specification))state->startup->requestStop();
        const bool prepared=state->capture->prepare();
        std::vector<std::string> argumentStorage{launch.executable};
        auto arguments=launch.specification->arguments();
        if constexpr(Socket){
            auto managed=launch.specification->managedArguments(nonce,state->capture->path());
            if(managed)arguments=std::move(*managed);else state->startup->requestStop();
        }
        argumentStorage.insert(argumentStorage.end(),arguments.begin(),arguments.end());
        std::vector<std::string> environment;
        for(const auto &item:launch.environment)
            if(item.rfind("PRO40_ADMISSION_TEST_NONCE=",0)!=0 && item.rfind("PRO40_ADMISSION_TEST_FD=",0)!=0)environment.push_back(item);
        char encoded[33]{};for(size_t i=0;i<nonce.size();++i)std::snprintf(encoded+i*2,3,"%02x",nonce[i]);
        if constexpr(!Socket){environment.push_back(std::string("PRO40_ADMISSION_TEST_NONCE=")+encoded);environment.push_back("PRO40_ADMISSION_TEST_FD=10");}
        std::vector<char*> argv,env;for(auto &a:argumentStorage)argv.push_back(a.data());argv.push_back(nullptr);
        for(auto &e:environment)env.push_back(e.data());env.push_back(nullptr);
        if(!prepared)state->startup->requestStop();
        mach_port_t bootstrap=MACH_PORT_NULL;
        if constexpr(!Socket){if(prepared)bootstrap=state->capture->bootstrap();}
        (void)state->startup->start(bootstrap,launch.errorFD,
            launch.executable.c_str(),argv.data(),env.data(),Timed?Pro40::ManagedRunTiming::collectionDeadline(began,launch.lifetime):std::optional(std::chrono::steady_clock::now()+Pro40::WorkerDiagnosticTiming::collection),
            Socket?Pro40::WorkerSpawnPreparation::Namespace::inherit:Pro40::WorkerSpawnPreparation::Namespace::replacementBootstrap);
        if(prepared)(void)state->capture->start(state->startup,std::chrono::steady_clock::now()+Pro40::WorkerDiagnosticTiming::capture);
        else state->maybeFinish();
    },std::move(stop)};
#endif
}
inline ExperimentControl makeNativeWorkerDiagnosticCommand(dispatch_queue_t queue,NSString *path,NSString *attempt,
    std::function<void(NSDictionary *)> report,NativeWorkerDiagnosticLaunch launch,bool stopBeforeDecision=false){
    return makeWorkerDiagnosticCommandImpl<false>(queue,path,attempt,std::move(report),std::move(launch),stopBeforeDecision);
}
inline ExperimentControl makeManagedSocketDiagnosticCommand(dispatch_queue_t queue,NSString *path,NSString *attempt,
    std::function<void(NSDictionary *)> report,NativeWorkerDiagnosticLaunch launch,bool stopBeforeDecision=false){
#ifdef PRO40_ENABLE_MANAGED_SOCKET_DIAGNOSTIC
    return makeWorkerDiagnosticCommandImpl<true>(queue,path,attempt,std::move(report),std::move(launch),stopBeforeDecision);
#else
    (void)queue;(void)path;(void)attempt;(void)report;(void)launch;(void)stopBeforeDecision;return {};
#endif
}
// Selected by the timed NativeStub and separately gated finite real-recording
// packages. Callers must use ManagedRunTiming::command for their outer envelope.
inline ExperimentControl makeManagedTimedDiagnosticCommand(dispatch_queue_t queue,NSString *path,NSString *attempt,
    std::function<void(NSDictionary *)> report,NativeWorkerDiagnosticLaunch launch,bool stopBeforeDecision=false){
#ifdef PRO40_ENABLE_MANAGED_TIMED_DIAGNOSTIC
    return makeWorkerDiagnosticCommandImpl<true,true>(queue,path,attempt,std::move(report),std::move(launch),stopBeforeDecision);
#else
    (void)queue;(void)path;(void)attempt;(void)report;(void)launch;(void)stopBeforeDecision;return {};
#endif
}

}
