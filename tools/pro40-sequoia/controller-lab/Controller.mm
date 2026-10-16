#include "LabIdentity.hpp"
#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>
#include "NativeRegistrationAttempt.hpp"
#include <cstring>
#include "ExperimentCommand.hpp"
#include "NativeDevelopmentIdentity.hpp"
#include "DevelopmentConnectionCommand.hpp"
#include "NativeVerifierSession.hpp"
#include "NativeVerifierCommand.hpp"
#include "BtmTimingDiagnostic.hpp"
#include "ControllerDuplexDiagnostic.hpp"
#include "ControllerDelegatedCommand.hpp"
#ifdef PRO40_ENABLE_DUPLEX_DELEGATED
#include "DuplexProvisionClientPins.hpp"
#endif

static Pro40Lab::ServiceStatus mapped(SMAppServiceStatus status) {
    switch (status) {
        case SMAppServiceStatusNotRegistered: return Pro40Lab::ServiceStatus::notRegistered;
        case SMAppServiceStatusEnabled: return Pro40Lab::ServiceStatus::enabled;
        case SMAppServiceStatusRequiresApproval: return Pro40Lab::ServiceStatus::requiresApproval;
        case SMAppServiceStatusNotFound: return Pro40Lab::ServiceStatus::notFound;
    }
    return Pro40Lab::ServiceStatus::unknown;
}
static int preflight(bool verifier = false) {
    NSString *journalPath = [NSHomeDirectory() stringByAppendingPathComponent:
        @PRO40_LAB_JOURNAL];
    Pro40Lab::RegistrationJournal journal;
    bool clear = journal.open(journalPath.fileSystemRepresentation, true) && journal.requireClear();
    NSMutableDictionary *report = [(verifier ? Pro40Lab::collectNativeVerifierRegistrationReport() :
        Pro40Lab::collectNativeRegistrationReport()) mutableCopy];
    report[@"requested_profile"] = verifier ? @"on-demand" : @"dormant";
    Pro40Lab::RegistrationPolicy policy;
    report[@"policy_prepared"] = @(policy.prepare(clear,
        mapped(static_cast<SMAppServiceStatus>([report[@"service_raw_status"] integerValue])),
        [report[@"intent_candidate_valid"] boolValue]));
    report[@"journal_path"] = journalPath;
    report[@"journal_clear"] = @(clear);
    report[@"journal_error"] = [NSString stringWithUTF8String:journal.error.c_str()];
    NSData *json = [NSJSONSerialization dataWithJSONObject:report
        options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nullptr];
    if (!json || fwrite(json.bytes, 1, json.length, stdout) != json.length || fputc('\n', stdout) == EOF) return 74;
    return [report[@"intent_candidate_valid"] boolValue] ? 0 : 2;
}
static int audit() {
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:
        @PRO40_LAB_JOURNAL];
    Pro40Lab::RegistrationJournal journal;
    NSDictionary *records = journal.open(path.fileSystemRepresentation, true) ? journal.audit() : nil;
    NSDictionary *report = @{@"mode": @"read-only-journal-audit", @"journal_path": path,
        @"available": @(records != nil), @"audit": records ?: @{},
        @"error": [NSString stringWithUTF8String:journal.error.c_str()],
        @"recovery_authorized": @NO};
    NSData *json = [NSJSONSerialization dataWithJSONObject:report
        options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nullptr];
    if (!json || fwrite(json.bytes, 1, json.length, stdout) != json.length || fputc(10, stdout) == EOF) return 74;
    return records ? 0 : 2;
}
static int recoveryCommand(NSString *attempt, NSString *hash, bool execute) {
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:
        @PRO40_LAB_JOURNAL];
    Pro40Lab::RegistrationRecoveryOwner owner;
    const auto result = execute ? Pro40Lab::recoverNativeRemoval(owner,
        path.fileSystemRepresentation, attempt, hash, [] { return false; }) :
        owner.preview(path.fileSystemRepresentation, attempt, hash,
        Pro40Lab::probeOriginalController,
        [] { return Pro40Lab::collectNativeRegistrationReport()[@"intent_candidate"]; },
        [] { return Pro40Lab::RegistrationRecoveryOwner::Clock::now(); }, [] { return false; });
    NSString *reason;
    switch (result) {
        case Pro40Lab::RecoveryResult::recovered: reason = @"recovery-record-persisted"; break;
        case Pro40Lab::RecoveryResult::persistenceFailed: reason = @"persistence-failed-reinspect-journal"; break;
        case Pro40Lab::RecoveryResult::eligible: reason = @"eligible-at-observation-time"; break;
        case Pro40Lab::RecoveryResult::unavailable: reason = @"journal-or-selected-record-unavailable"; break;
        case Pro40Lab::RecoveryResult::ownerUnresolved: reason = @"original-controller-live-or-unknown"; break;
        case Pro40Lab::RecoveryResult::invalidObservation: reason = @"fresh-observation-invalid-or-identity-mismatch"; break;
        case Pro40Lab::RecoveryResult::expired: reason = @"observation-budget-expired"; break;
        case Pro40Lab::RecoveryResult::stopped: reason = @"stopped"; break;
        default: reason = @"recovery-refused"; break;
    }
    NSDictionary *report = @{@"mode": execute ? @"explicit-record-recovery" : @"read-only-recovery-preview", @"journal_path": path,
        @"attempt": attempt ?: @"", @"record_sha256": hash ?: @"",
        @"eligible_now": @(result == Pro40Lab::RecoveryResult::eligible), @"reason": reason,
        @"error": [NSString stringWithUTF8String:owner.error().c_str()],
        @"recovery_authorized": @NO, @"requires_fresh_execution_checks": @YES,
        @"recovery_record_persisted": @(result == Pro40Lab::RecoveryResult::recovered),
        @"persistence_outcome_uncertain": @(result == Pro40Lab::RecoveryResult::persistenceFailed),
        @"service_mutation_attempted": @NO};
    NSData *json = [NSJSONSerialization dataWithJSONObject:report
        options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys error:nullptr];
    if (!json || fwrite(json.bytes, 1, json.length, stdout) != json.length || fputc(10, stdout) == EOF) return 74;
    return (execute ? result == Pro40Lab::RecoveryResult::recovered :
        result == Pro40Lab::RecoveryResult::eligible) ? 0 : 2;
}
static int dormantExperiment() {
    NSString *path = [NSHomeDirectory() stringByAppendingPathComponent:
        @PRO40_LAB_JOURNAL];
    NSString *attempt = [[NSUUID.UUID.UUIDString lowercaseString] stringByReplacingOccurrencesOfString:@"-" withString:@""];
    return Pro40Lab::runExperimentCommand([attempt, path](dispatch_queue_t queue, auto report) {
        auto native = Pro40Lab::makeNativeRegistrationExperiment(queue, report);
        return Pro40Lab::ExperimentControl{
            [loop = native.loop, collect = native.collect, attempt, path] {
                (void)loop->start(path.fileSystemRepresentation, attempt, collect);
            }, std::move(native.requestStop)};
    }, attempt, path);
}
static int verifierStartupRefusal() {
    auto queue=dispatch_queue_create("pro40.verifier-startup-refusal",DISPATCH_QUEUE_SERIAL);
    NSString *path=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    __block bool blocked=false,session=false,callback=false;
    dispatch_sync(queue,^{
        auto resources=Pro40Lab::makeNativeVerifierResources(queue);
        Pro40Lab::LaunchNonce nonce{};nonce[0]=1;
        auto callbackSlot=&callback;
        auto result=Pro40Lab::registerAndPrepareNativeVerifierSession(queue,resources,path.fileSystemRepresentation,
            @"0123456789abcdef0123456789abcdef",nonce,Pro40Lab::observationNS()+5000000000,
            []{return true;},[callbackSlot](NSDictionary *){*callbackSlot=true;});
        blocked=result.registration==Pro40Lab::VerifierRegistrationResult::blocked;
        session=bool(result.binding.session);
    });
    const bool created=[NSFileManager.defaultManager fileExistsAtPath:path];
    NSDictionary *report=@{@"mode":@"verifier-startup-refusal",@"blocked":@(blocked),@"session_created":@(session),
        @"callback_received":@(callback),@"journal_created":@(created),@"service_mutation_attempted":@NO};
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
    if(!json || fwrite(json.bytes,1,json.length,stdout)!=json.length || fputc('\n',stdout)==EOF)return 74;
    return blocked && !session && !callback && !created?0:3;
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        if(argc>=2 && std::strcmp(argv[1],"--run-managed-duplex-delegated")==0) {
#ifdef PRO40_ENABLE_DUPLEX_DELEGATED
            uint32_t generation=7;
#if defined(PRO40_ENABLE_DUPLEX_HARDWARE) && !defined(PRO40_SESSION_PROFILE)
            if(argc!=4)return 64;
            const std::string_view text(argv[3]);const auto parsed=std::from_chars(text.data(),text.data()+text.size(),generation);
            if(text.empty() || parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())return 64;
#else
            if(argc!=3)return 64;
#endif
            SecRequirementRef broker=nullptr,helper=nullptr;
            if(SecRequirementCreateWithString(CFSTR(PRO40_PROVISION_BROKER_REQUIREMENT),0,&broker)!=errSecSuccess)return 65;
            Pro40Audio::IdentityDetail::Owned<SecRequirementRef> ownBroker(broker);
            if(SecRequirementCreateWithString(CFSTR(PRO40_PROVISION_HELPER_REQUIREMENT),0,&helper)!=errSecSuccess)return 65;
            Pro40Audio::IdentityDetail::Owned<SecRequirementRef> ownHelper(helper);
            return Pro40Lab::controllerDelegatedCommand(argv[2],PRO40_PROVISION_SERVICE,broker,helper,generation);
#else
            fputs("delegated duplex is not enabled\n",stderr);return 78;
#endif
        }
        if(argc==4 && (std::strcmp(argv[1],"--run-managed-duplex-hardware")==0 || std::strcmp(argv[1],"--run-managed-duplex-hardware-prestop")==0)) {
#ifdef PRO40_ENABLE_DUPLEX_HARDWARE
            uint32_t generation=0;const std::string_view text(argv[3]);const auto parsed=std::from_chars(text.data(),text.data()+text.size(),generation);
            if(text.empty() || parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())return 64;
            return Pro40Lab::controllerDuplexDiagnostic(argv[2],std::strcmp(argv[1],"--run-managed-duplex-hardware-prestop")==0,generation);
#else
            fputs("hardware duplex is not enabled\n",stderr);return 78;
#endif
        }
        if(argc==3 && (std::strcmp(argv[1],"--run-managed-duplex-diagnostic")==0 || std::strcmp(argv[1],"--run-managed-duplex-prestop")==0))
#ifdef PRO40_ENABLE_DUPLEX_HARDWARE
            {fputs("generated duplex is not enabled in hardware profile\n",stderr);return 78;}
#else
            return Pro40Lab::controllerDuplexDiagnostic(argv[2],std::strcmp(argv[1],"--run-managed-duplex-prestop")==0);
#endif
        if(argc==2 && (std::strcmp(argv[1],"--run-gated-worker-diagnostic")==0 ||
                      std::strcmp(argv[1],"--run-managed-socket-diagnostic")==0 ||
                      std::strcmp(argv[1],"--run-managed-timed-diagnostic")==0 ||
                      std::strcmp(argv[1],"--run-managed-real-recording")==0 ||
                      std::strcmp(argv[1],"--check-gated-worker-terminal-stop")==0)){
#if defined(PRO40_MANAGED_REAL_INSPECT) || defined(PRO40_ENABLE_DUPLEX_FIXTURE) || defined(PRO40_ENABLE_DUPLEX_HARDWARE)
            fputs("worker launch is not enabled in real inspection package\n",stderr);return 78;
#elif defined(PRO40_ENABLE_GATED_WORKER_DIAGNOSTIC) && defined(PRO40_ENABLE_ORIGINAL_PEER_DIAGNOSTIC)
            const bool realMode=std::strcmp(argv[1],"--run-managed-real-recording")==0;
#ifdef PRO40_ENABLE_MANAGED_REAL_RECORDING
            if(!realMode){fputs("diagnostic worker mode is not enabled in real recording package\n",stderr);return 78;}
#else
            if(realMode){fputs("real recording is not enabled\n",stderr);return 78;}
#endif
            const bool timedMode=realMode || std::strcmp(argv[1],"--run-managed-timed-diagnostic")==0;
            const bool socketMode=timedMode || std::strcmp(argv[1],"--run-managed-socket-diagnostic")==0;
#ifdef PRO40_ENABLE_MANAGED_TIMED_DIAGNOSTIC
            if(!timedMode){fputs("untimed worker diagnostic is not enabled in timed package\n",stderr);return 78;}
#else
            if(timedMode){fputs("managed timed diagnostic is not enabled\n",stderr);return 78;}
#endif
#ifdef PRO40_ENABLE_MANAGED_SOCKET_DIAGNOSTIC
            if(!socketMode){fputs("legacy worker diagnostic is not enabled in managed package\n",stderr);return 78;}
#else
            if(socketMode){fputs("managed socket diagnostic is not enabled\n",stderr);return 78;}
#endif
            if(!Pro40Lab::developmentOriginalPeerEnabled ||
               !Pro40Lab::developmentPeerRequirement(Pro40Lab::DevelopmentRole::controller))return 2;
            std::string pattern=std::string(NSTemporaryDirectory().fileSystemRepresentation)+"pro40-worker-diagnostic-XXXXXX";
            std::vector<char> temporary(pattern.begin(),pattern.end());temporary.push_back(0);
            if(!mkdtemp(temporary.data()))return 74;
            const std::string folder=temporary.data(),leases=folder+"/leases",log=folder+"/session.log";
            if(!realMode && mkdir(leases.c_str(),0700))return 74;
            const int errors=open((folder+"/worker.err").c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);
            if(errors<0)return 74;
            Pro40Lab::NativeWorkerDiagnosticLaunch launch;
            launch.executable=[NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Contents/MacOS/Pro40DiagnosticWorker"].fileSystemRepresentation;
#ifdef PRO40_ENABLE_MANAGED_REAL_RECORDING
            static_assert(PRO40_MANAGED_REAL_GUID!=0);
            launch.specification=Pro40::WorkerLaunchSpecification::create(PRO40_MANAGED_REAL_GUID,log,
#if defined(PRO40_MANAGED_REAL_ENABLE_FIRST) && defined(PRO40_MANAGED_REAL_EXISTING_CLOCK)
#error Enable-first and existing-clock diagnostics cannot be combined
#endif
#if defined(PRO40_MANAGED_REAL_CLOCK_EXCURSION) && (!defined(PRO40_MANAGED_REAL_ENABLE_FIRST) || defined(PRO40_MANAGED_REAL_EXISTING_CLOCK))
#error Clock excursion requires enable-first and excludes existing-clock mode
#endif
#ifdef PRO40_MANAGED_REAL_CLOCK_EXCURSION
                Pro40::WorkerLaunchSpecification::Operation::recordClockExcursion);
#elif defined(PRO40_MANAGED_REAL_ENABLE_FIRST)
                Pro40::WorkerLaunchSpecification::Operation::recordEnableFirst);
#elif defined(PRO40_MANAGED_REAL_EXISTING_CLOCK)
                Pro40::WorkerLaunchSpecification::Operation::recordExistingClock);
#elif defined(PRO40_MANAGED_REAL_ASSIGNED_IDLE)
                Pro40::WorkerLaunchSpecification::Operation::recordAssignedIdle);
#elif defined(PRO40_MANAGED_REAL_OWNED_IDLE)
                Pro40::WorkerLaunchSpecification::Operation::recordOwnedIdle);
#elif defined(PRO40_MANAGED_REAL_QUIET_HOLD)
                Pro40::WorkerLaunchSpecification::Operation::recordQuietHold);
#elif defined(PRO40_MANAGED_REAL_POST_RELEASE)
                Pro40::WorkerLaunchSpecification::Operation::recordPostRelease);
#elif defined(PRO40_MANAGED_REAL_RECORD_TIMING)
                Pro40::WorkerLaunchSpecification::Operation::recordTiming);
#else
                Pro40::WorkerLaunchSpecification::Operation::recordInput);
#endif
#else
            launch.specification=Pro40::WorkerLaunchSpecification::create(42,log);
#endif
            launch.epoch=Pro40Lab::observationNS();launch.errorFD=errors;
            for(char **item=environ;*item;++item)if(std::strncmp(*item,"PRO40_",6)!=0)launch.environment.emplace_back(*item);
            if(!realMode)launch.environment.push_back("PRO40_WORKER_TEST_LOCK_DIR="+leases);
            NSString *output=[NSString stringWithUTF8String:folder.c_str()];
            NSString *path=[NSHomeDirectory() stringByAppendingPathComponent:@PRO40_LAB_JOURNAL];
            NSString *attempt=[[NSUUID.UUID.UUIDString lowercaseString] stringByReplacingOccurrencesOfString:@"-" withString:@""];
            const bool terminalStop=std::strcmp(argv[1],"--check-gated-worker-terminal-stop")==0;
            const int result=Pro40Lab::runExperimentCommand([path,attempt,output,launch,terminalStop,socketMode,timedMode,realMode](dispatch_queue_t queue,auto report){
                auto annotatedReport=[report,output,realMode](NSDictionary *value){
                    NSMutableDictionary *annotated=[value mutableCopy];annotated[@"diagnostic_output"]=output;
                    annotated[@"real_worker_profile"]=@(realMode);report(annotated);
                };
                if(timedMode)return Pro40Lab::makeManagedTimedDiagnosticCommand(queue,path,attempt,annotatedReport,launch);
                if(socketMode)return Pro40Lab::makeManagedSocketDiagnosticCommand(queue,path,attempt,annotatedReport,launch);
                return Pro40Lab::makeNativeWorkerDiagnosticCommand(queue,path,attempt,annotatedReport,launch,terminalStop);
            },attempt,path,timedMode?Pro40::ManagedRunTiming::command:Pro40::WorkerDiagnosticTiming::command,
                realMode?@"managed-real-recording":timedMode?@"managed-timed-diagnostic":@"gated-worker-diagnostic");
            if(close(errors)<0)return 74;return result;
#else
            fputs("gated worker diagnostic is not enabled\n",stderr);return 78;
#endif
        }
        if((argc==3 || argc==4) && std::strcmp(argv[1],"--check-btm-timing")==0){
#ifdef PRO40_ENABLE_ORIGINAL_PEER_DIAGNOSTIC
            if(!Pro40Lab::developmentPeerRequirement(Pro40Lab::DevelopmentRole::controller))return 2;
            if(argc==3 && std::strcmp(argv[2],"main")==0)
                return Pro40Lab::reportBtmTiming(std::chrono::seconds(7));
            if(std::strcmp(argv[2],"owner")!=0)return 64;
            if(argc==4 && argv[3][0]!='/')return 64;
            const std::string sender=argc==4?argv[3]:"";
            auto queue=dispatch_queue_create("pro40.btm-timing",DISPATCH_QUEUE_SERIAL);
            auto done=dispatch_semaphore_create(0);
            __block int result=2;
            dispatch_async(queue,^{@autoreleasepool{
                std::unique_ptr<Pro40Lab::TestCapturedIdentityPeer> peer;
                if(!sender.empty())peer=std::make_unique<Pro40Lab::TestCapturedIdentityPeer>(sender.c_str());
                result=Pro40Lab::reportBtmTiming(std::chrono::seconds(7));
                if(peer){
                    const auto pid=peer->pid();peer->reap();
                    printf("{\"original_peer_pid\":%d,\"original_peer_reaped\":true}\n",pid);
                }
                dispatch_semaphore_signal(done);
            }});
            if(dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,16*NSEC_PER_SEC)))return 75;
            return result;
#else
            fputs("BTM timing diagnostic is not enabled\n",stderr);return 78;
#endif
        }
        if(argc==3 && (std::strcmp(argv[1],"--run-original-peer-diagnostic")==0 ||
                      std::strcmp(argv[1],"--check-original-peer-prestop")==0 ||
                      std::strcmp(argv[1],"--check-original-peer-terminal-stop")==0)){
#ifdef PRO40_ENABLE_ORIGINAL_PEER_DIAGNOSTIC
            if(!Pro40Lab::developmentOriginalPeerEnabled || argv[2][0]!='/' ||
               !Pro40Lab::developmentPeerRequirement(Pro40Lab::DevelopmentRole::controller))return 2;
            NSString *path=[NSHomeDirectory() stringByAppendingPathComponent:@PRO40_LAB_JOURNAL];
            NSString *attempt=[[NSUUID.UUID.UUIDString lowercaseString] stringByReplacingOccurrencesOfString:@"-" withString:@""];
            const std::string sender=argv[2];
            const bool prestop=std::strcmp(argv[1],"--check-original-peer-prestop")==0;
            const bool terminalStop=std::strcmp(argv[1],"--check-original-peer-terminal-stop")==0;
            return Pro40Lab::runExperimentCommand([path,attempt,sender,prestop,terminalStop](dispatch_queue_t queue,auto report){
                auto control=Pro40Lab::makeNativeVerifierCommand(queue,path,attempt,report,sender,terminalStop);
                if(prestop)control.stop(); // Real command stop before its queued start.
                return control;
            },attempt,path,Pro40Lab::nativeVerifierCommandLimit,@"original-peer-diagnostic");
#else
            fputs("original-peer diagnostic is not enabled\n",stderr);return 78;
#endif
        }
        if(argc==2 && std::strcmp(argv[1],"--check-original-peer-policy")==0)
            return Pro40Lab::developmentOriginalPolicyCommand(Pro40Lab::DevelopmentRole::controller);
        if(argc==2 && std::strcmp(argv[1],"--run-verifier-experiment")==0){
#ifdef PRO40_ENABLE_VERIFIER_EXPERIMENT
            NSString *path=[NSHomeDirectory() stringByAppendingPathComponent:@PRO40_LAB_JOURNAL];
            NSString *attempt=[[NSUUID.UUID.UUIDString lowercaseString] stringByReplacingOccurrencesOfString:@"-" withString:@""];
            return Pro40Lab::runExperimentCommand([path,attempt](dispatch_queue_t queue,auto report){
                return Pro40Lab::makeNativeVerifierCommand(queue,path,attempt,report);
            },attempt,path,Pro40Lab::nativeVerifierCommandLimit,@"verifier-command");
#else
            fputs("on-demand verifier lifecycle is not enabled\n",stderr);return 78;
#endif
        }
        if(argc==2 && std::strcmp(argv[1],"--check-verifier-supervisor-refusal")==0){
            NSString *path=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
            return Pro40Lab::runExperimentCommand([path](dispatch_queue_t queue,auto report){
                auto control=Pro40Lab::makeNativeVerifierCommand(queue,path,@"0123456789abcdef0123456789abcdef",report);
                control.stop(); // diagnostic cannot enter registration
                return control;
            },@"0123456789abcdef0123456789abcdef",path,Pro40Lab::nativeVerifierCommandLimit,@"verifier-command");
        }
        if(argc==4 && (std::strcmp(argv[1],"--preview-launch-recovery")==0 || std::strcmp(argv[1],"--recover-launch-failure")==0)){
            const bool execute=std::strcmp(argv[1],"--recover-launch-failure")==0;
            NSString *path=[NSHomeDirectory() stringByAppendingPathComponent:@PRO40_LAB_JOURNAL];
            Pro40Lab::LaunchRecoveryReviewOwner owner;
            auto now=[]{return Pro40Lab::LaunchRecoveryReviewOwner::Clock::now();};auto stop=[]{return false;};
            NSString *attempt=[NSString stringWithUTF8String:argv[2]],*hash=[NSString stringWithUTF8String:argv[3]];
            const auto result=execute?owner.recover(path.fileSystemRepresentation,attempt,hash,Pro40Lab::collectNativeLaunchRecoveryObservation,now,stop):
                owner.review(path.fileSystemRepresentation,attempt,hash,Pro40Lab::collectNativeLaunchRecoveryObservation,now,stop);
            NSString *reason=@"unavailable";
            switch(result){
                case Pro40Lab::LaunchRecoveryOwnerResult::eligibleAfterReboot:reason=@"eligible-after-reboot";break;
                case Pro40Lab::LaunchRecoveryOwnerResult::currentBoot:reason=@"same-boot-unresolved";break;
                case Pro40Lab::LaunchRecoveryOwnerResult::invalidObservation:reason=@"invalid-observation";break;
                case Pro40Lab::LaunchRecoveryOwnerResult::expired:reason=@"expired";break;
                case Pro40Lab::LaunchRecoveryOwnerResult::stopped:reason=@"stopped";break;
                case Pro40Lab::LaunchRecoveryOwnerResult::refused:reason=@"refused";break;
                case Pro40Lab::LaunchRecoveryOwnerResult::recovered:reason=@"recovery-record-persisted";break;
                case Pro40Lab::LaunchRecoveryOwnerResult::persistenceFailed:reason=@"persistence-failed-reinspect-journal";break;
                default:break;
            }
            const bool recovered=result==Pro40Lab::LaunchRecoveryOwnerResult::recovered;
            const bool uncertain=result==Pro40Lab::LaunchRecoveryOwnerResult::persistenceFailed;
            NSDictionary *report=@{@"mode":execute?@"explicit-launch-recovery":@"read-only-launch-recovery-preview",@"reason":reason,
                @"eligible_now":@(result==Pro40Lab::LaunchRecoveryOwnerResult::eligibleAfterReboot),
                @"recovery_authorized":@(owner.writeAttempted()),@"record_modified":uncertain?(id)[NSNull null]:@(recovered),
                @"write_attempted":@(owner.writeAttempted()),@"reinspection_required":@(uncertain),@"service_mutation_attempted":@NO,
                @"error":[NSString stringWithUTF8String:owner.error().c_str()]};
            NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
            if(!json || fwrite(json.bytes,1,json.length,stdout)!=json.length || fputc('\n',stdout)==EOF)return 74;
            return recovered || result==Pro40Lab::LaunchRecoveryOwnerResult::eligibleAfterReboot?0:uncertain?75:2;
        }
        if (argc == 2 && std::strcmp(argv[1], "--inspect-verifier-removal") == 0) {
            NSDictionary *identity=Pro40Lab::collectNativeRegistrationReport()[@"intent_candidate"][@"identity"];
            NSDictionary *report=Pro40Lab::collectNativeVerifierRemovalReport(identity);
            NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
            if(!json || fwrite(json.bytes,1,json.length,stdout)!=json.length || fputc('\n',stdout)==EOF)return 74;
            return 0; // diagnostic output, not permission or a claim of valid removal
        }
        if (argc == 2 && std::strcmp(argv[1], "--check-controller-preparation") == 0)
            return Pro40Lab::developmentControllerPreparationCommand();
        if (argc == 2 && std::strcmp(argv[1], "--check-verifier-preparation") == 0)
            return Pro40Lab::developmentConnectionCommand(false);
        if (argc == 2 && std::strcmp(argv[1], "--check-development-identity") == 0)
            return Pro40Lab::developmentIdentityCommand(Pro40Lab::DevelopmentRole::controller);
        if (argc == 2 && std::strcmp(argv[1], "--preflight") == 0) return preflight();
        if (argc == 2 && std::strcmp(argv[1], "--preflight-verifier") == 0) return preflight(true);
        if (argc == 2 && std::strcmp(argv[1], "--check-verifier-startup-refusal") == 0) return verifierStartupRefusal();
        if (argc == 2 && std::strcmp(argv[1], "--audit") == 0) return audit();
        if (argc == 2 && std::strcmp(argv[1], "--register-dormant-experiment") == 0) return dormantExperiment();
        if (argc == 4 && std::strcmp(argv[1], "--preview-recovery") == 0)
            return recoveryCommand([NSString stringWithUTF8String:argv[2]], [NSString stringWithUTF8String:argv[3]], false);
        if (argc == 4 && std::strcmp(argv[1], "--recover-removal") == 0)
            return recoveryCommand([NSString stringWithUTF8String:argv[2]], [NSString stringWithUTF8String:argv[3]], true);
        if (argc != 1) return 64;
        [NSApplication sharedApplication];
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = @"Pro40 Controller Lab";
        alert.informativeText = @"Development build with read-only diagnostics and explicit journal recovery. Dormant registration is available only through the explicit experiment command. No audio operations are enabled.";
        [alert addButtonWithTitle:@"Close"];
        [alert runModal];
    }
    return 0;
}
