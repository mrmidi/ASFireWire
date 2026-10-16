// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ControllerDelegatedSession.hpp"
#include "DelegatedSessionOutcome.hpp"
#include "NativeWorkerDiagnosticCommand.hpp"
#include "NativeDevelopmentIdentity.hpp"
#include "../DuplexProvisionClient.hpp"
#include "../NativeDuplexPolicy.hpp"
#if defined(PRO40_SESSION_PROFILE) && defined(PRO40_ENABLE_DUPLEX_HARDWARE)
#include "ControllerDeviceState.hpp"
#endif
extern char **environ;
namespace Pro40Lab {
#ifdef PRO40_ENABLE_DUPLEX_DELEGATED
#if !defined(PRO40_ENABLE_DUPLEX_FIXTURE) && !defined(PRO40_ENABLE_DUPLEX_HARDWARE)
#error Delegated command requires an explicit generated or hardware duplex profile.
#endif
// One bounded session in the existing signed command envelope. The broker
// requirements and name are trusted build configuration, never CLI/request
// supplied policy. This does not register a permanent system service.
inline int controllerDelegatedCommand(const char *workerService,const char *brokerService,
    SecRequirementRef expectedBroker,SecRequirementRef expectedHelper,uint32_t generation) {
    using namespace Pro40Audio;using namespace std::chrono_literals;
#ifdef PRO40_SESSION_PROFILE
#if defined(PRO40_APPLICATION_TRIAL) || defined(PRO40_ENDURANCE_TRIAL)
#error Session lifetime must not be mixed with a finite application/endurance trial.
#endif
    constexpr auto lifetime=Pro40::ManagedRunTiming::Lifetime::untilStopped;
#else
    constexpr auto lifetime=Pro40::ManagedRunTiming::Lifetime::bounded;
#endif
    ExperimentSignals signals;if(!signals.ready())return 71;
    if(!developmentOriginalPeerEnabled || !developmentPeerRequirement(DevelopmentRole::controller) || !expectedBroker || !expectedHelper)return 2;
    const std::string service(workerService),prefix=std::string(PRO40_LAB_APP_ID)+".audio.";
    if(!service.starts_with(prefix) || service.size()!=prefix.size()+32 || service.find_first_not_of("0123456789abcdef",prefix.size())!=std::string::npos)return 64;
    NativeMachPort workerListener(MACH_PORT_RIGHT_RECEIVE),listener(MACH_PORT_RIGHT_RECEIVE);mach_port_t raw=MACH_PORT_NULL;
    if(bootstrap_check_in(bootstrap_port,service.c_str(),&raw)!=KERN_SUCCESS || workerListener.adopt(raw)!=KERN_SUCCESS || listener.allocate()!=KERN_SUCCESS)return 74;
    mach_timebase_info_data_t tb{};if(mach_timebase_info(&tb)!=KERN_SUCCESS)return 74;
    const auto registration=ProvisionClient::nonce();
    if(ProvisionClient::registration(brokerService,expectedBroker,registration,listener.get())!=DuplexProvisionRegistry::Result::ok)return 74;
    std::printf("{\"delegated_controller_registered\":true,\"native_started\":false,\"pid\":%d}\n",getpid());std::fflush(stdout);
    std::unique_ptr<DuplexHostWire::Request> request;
#ifdef PRO40_SESSION_PROFILE
    const std::optional<std::chrono::steady_clock::time_point> deadline=std::nullopt;
#elif defined(PRO40_APPLICATION_TRIAL)
    const std::optional deadline=std::chrono::steady_clock::now()+10min;
#else
    const std::optional deadline=std::chrono::steady_clock::now()+60s;
#endif
    while((!deadline || std::chrono::steady_clock::now()<*deadline) && !request && !experimentSignal) {
        NativeMachReceived<512> message;
        if(message.read(listener.get(),20,nativeAuditReceiveOptions))request=DuplexHostWire::accept(message,listener.get(),expectedHelper,tb.numer,tb.denom);
        else if(message.status()!=MACH_RCV_TIMED_OUT)break;
    }
    const auto removed=ProvisionClient::registration(brokerService,expectedBroker,registration);
    const bool listenerClosed=listener.close()==KERN_SUCCESS;
    if(!request)return removed==DuplexProvisionRegistry::Result::ok && listenerClosed?75:74;
    if(removed!=DuplexProvisionRegistry::Result::ok || !listenerClosed) {
        (void)DuplexHostWire::joined(request->completion.get(),request->values.nonce,false);return 74;
    }
#if defined(PRO40_SESSION_PROFILE) && defined(PRO40_ENABLE_DUPLEX_HARDWARE)
    // Only after authenticated demand, before verifier registration/native
    // admission. Native code still rejects any reset after this observation.
    const auto state=currentSessionDeviceState();
    if(!state || experimentSignal) {
        const bool replyClosed=request->reply.close()==KERN_SUCCESS;
        const bool audioClosed=request->audio.close()==KERN_SUCCESS;
        const bool workerClosed=workerListener.close()==KERN_SUCCESS;
        const bool clean=replyClosed && audioClosed && workerClosed;
        (void)DuplexHostWire::joined(request->completion.get(),request->values.nonce,clean);
        std::printf("{\"session_device_state_unavailable\":true,\"native_started\":false,\"local_cleanup_complete\":%s}\n",clean?"true":"false");
        return 69;
    }
    generation=state->generation;
    std::printf("{\"session_device_generation\":%u,\"session_device_node\":%u,\"native_started\":false}\n",generation,state->node);std::fflush(stdout);
#endif
    const uint64_t epoch=observationNS(),seed=request->values.seed,nonce=ProvisionClient::nonce();
    auto context=std::make_shared<ControllerDelegatedSession>(std::move(request),SourceFrameClock::Identity{epoch,seed,generation,tb.numer,tb.denom},
        lifetime==Pro40::ManagedRunTiming::Lifetime::bounded?std::optional<std::chrono::milliseconds>(Pro40::ManagedRunTiming::run):std::nullopt);
    std::string pattern=std::string(NSTemporaryDirectory().fileSystemRepresentation)+"pro40-delegated-diagnostic-XXXXXX";
    std::vector<char> temporary(pattern.begin(),pattern.end());temporary.push_back(0);
    if(!mkdtemp(temporary.data())){(void)context->finishAfterJoin(true);return 74;}
    const std::string folder=temporary.data(),leases=folder+"/leases",log=folder+"/session.log";
#ifdef PRO40_ENABLE_DUPLEX_FIXTURE
    if(mkdir(leases.c_str(),0700)){(void)context->finishAfterJoin(true);return 74;}
#endif
    const int errors=open((folder+"/worker.err").c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);
    if(errors<0){(void)context->finishAfterJoin(true);return 74;}
    NativeWorkerDiagnosticLaunch launch;
    launch.lifetime=lifetime;
    launch.executable=[NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Contents/MacOS/Pro40DiagnosticWorker"].fileSystemRepresentation;
    launch.epoch=epoch;launch.errorFD=errors;
#ifdef PRO40_ENABLE_DUPLEX_HARDWARE
    constexpr uint64_t guid=PRO40_MANAGED_REAL_GUID;
#else
    constexpr uint64_t guid=0x00130e0400000001;
#endif
    launch.specification=Pro40::WorkerLaunchSpecification::createDuplex(guid,log,{service,epoch,nonce,seed,generation});
    for(char **item=environ;*item;++item)if(std::strncmp(*item,"PRO40_",6)!=0)launch.environment.emplace_back(*item);
#ifdef PRO40_ENABLE_DUPLEX_FIXTURE
    launch.environment.push_back("PRO40_WORKER_TEST_LOCK_DIR="+leases);
#endif
    NSString *output=[NSString stringWithUTF8String:folder.c_str()];
    NSString *journal=[NSHomeDirectory() stringByAppendingPathComponent:@PRO40_LAB_JOURNAL];
    NSString *attempt=[[NSUUID.UUID.UUIDString lowercaseString] stringByReplacingOccurrencesOfString:@"-" withString:@""];
    const auto policy=nativeDuplexClockPolicy(tb.numer,tb.denom);
    auto receipt=std::make_shared<std::atomic<bool>>(false),clean=std::make_shared<std::atomic<bool>>(false),restart=std::make_shared<std::atomic<bool>>(false);
    const int result=runExperimentCommand([launch,listener=std::move(workerListener),context,policy,epoch,seed,generation,tb,output,journal,attempt,receipt,clean,restart](dispatch_queue_t queue,auto report)mutable {
        WorkerDuplexConnectionOwner::Delegate delegate=[context](auto grant,audit_token_t worker,DuplexLaunchBinding binding,const auto &cancel) {
            return context->run(std::move(grant),worker,binding,cancel);
        };
        launch.duplex=std::make_shared<WorkerDuplexConnectionOwner>(queue,std::move(listener),SourceFrameClock::Identity{epoch,seed,generation,tb.numer,tb.denom},policy,std::move(delegate));
        auto annotated=[context,output,receipt,clean,restart,report](NSDictionary *value) {
            NSMutableDictionary *m=[value mutableCopy];m[@"diagnostic_output"]=output;m[@"delegated_session"]=@YES;
            if([value[@"supervision_finished"] boolValue]) {
                const bool released=[value[@"diagnostic_worker_released"] boolValue];
                const bool native=!released || ([value[@"native_outcome_observed"] boolValue] && [value[@"worker_result_observed"] boolValue] &&
                    [value[@"native_outcome"] isKindOfClass:NSDictionary.class] && [value[@"native_outcome"][@"cleanup"] unsignedIntValue]==0);
                const bool joined=native && [value[@"local_cleanup_complete"] boolValue] && [value[@"registration_reconciled"] boolValue] &&
                    [value[@"worker_entry_resolved"] boolValue] && [value[@"duplex_local_cleanup_complete"] boolValue];
                clean->store(joined);receipt->store(context->finishAfterJoin(joined));
                m[@"delegated_cleanup_confirmed"]=@(joined);m[@"delegated_cleanup_receipt_sent"]=@(receipt->load());
                const bool observed=[value[@"native_outcome_observed"] boolValue] && [value[@"native_outcome"] isKindOfClass:NSDictionary.class];
                const DelegatedSessionOutcome outcome{joined,receipt->load(),bool([value[@"duplex_stream_passed"] boolValue]),observed,
                    observed && [value[@"native_outcome"][@"evidence"] unsignedIntValue]==0,
                    observed && [value[@"native_outcome"][@"cleanup"] unsignedIntValue]==0,bool([value[@"worker_result_observed"] boolValue])};
                restart->store(outcome.restartPermitted());m[@"session_restart_permitted"]=@(restart->load());
            }
            report(m);
        };
        return makeManagedTimedDiagnosticCommand(queue,journal,attempt,annotated,launch);
    },attempt,journal,Pro40::ManagedRunTiming::commandLimit(lifetime),@"managed-delegated-duplex-diagnostic");
    const bool errorsClosed=close(errors)==0;
    return result?result:errorsClosed && receipt->load() && clean->load() && restart->load()?0:74;
}
#endif
}
