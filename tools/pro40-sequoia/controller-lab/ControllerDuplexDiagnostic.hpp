// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeWorkerDiagnosticCommand.hpp"
#include "NativeDevelopmentIdentity.hpp"
#include "../SustainedInputFixture.hpp"
#include <servers/bootstrap.h>
#include "DuplexHardwareRender.hpp"
#ifdef PRO40_ENABLE_DUPLEX_LOOPBACK
#include "DuplexLoopback.hpp"
#endif
#include "../NativeDuplexPolicy.hpp"
namespace Pro40Lab {
struct DuplexDiagnosticProgress {
    std::atomic<unsigned> block{0},failure{0}; // 1 cancel, 2 input, 3 samples, 4 output, 5 timestamp
    std::atomic<uint64_t> inputFirst{0},elapsedNS{0};
};
// Compile-time separated generated and real profiles, with the same retained
// child/verifier/audio ownership. Hardware launch requires an explicit generation.
inline int controllerDuplexDiagnostic(const char *serviceName,bool prestop,uint32_t generation=7){
#if !defined(PRO40_ENABLE_DUPLEX_FIXTURE) && !defined(PRO40_ENABLE_DUPLEX_HARDWARE)
    (void)serviceName;(void)prestop;(void)generation;fputs("duplex diagnostic is not enabled\n",stderr);return 78;
#else
    using namespace Pro40Audio;using namespace std::chrono_literals;
    if(!developmentOriginalPeerEnabled || !developmentPeerRequirement(DevelopmentRole::controller))return 2;
    const std::string service(serviceName),prefix=std::string(PRO40_LAB_APP_ID)+".audio.";
    if(!service.starts_with(prefix) || service.size()!=prefix.size()+32 ||
       service.find_first_not_of("0123456789abcdef",prefix.size())!=std::string::npos)return 64;
    NativeMachPort listener(MACH_PORT_RIGHT_RECEIVE);mach_port_t raw=MACH_PORT_NULL;
    if(bootstrap_check_in(bootstrap_port,service.c_str(),&raw)!=KERN_SUCCESS || listener.adopt(raw)!=KERN_SUCCESS)return 74;
    std::string pattern=std::string(NSTemporaryDirectory().fileSystemRepresentation)+"pro40-duplex-diagnostic-XXXXXX";
    std::vector<char> temporary(pattern.begin(),pattern.end());temporary.push_back(0);
    if(!mkdtemp(temporary.data()))return 74;
    const std::string folder=temporary.data(),leases=folder+"/leases",log=folder+"/session.log";
    #ifdef PRO40_ENABLE_DUPLEX_FIXTURE
    if(mkdir(leases.c_str(),0700))return 74;
#endif
    const int errors=open((folder+"/worker.err").c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);if(errors<0)return 74;
    mach_timebase_info_data_t tb{};if(mach_timebase_info(&tb)!=KERN_SUCCESS){close(errors);return 74;}
    const uint64_t epoch=observationNS();uint64_t nonce=0;do{arc4random_buf(&nonce,sizeof(nonce));}while(!nonce);
    NativeWorkerDiagnosticLaunch launch;
    launch.executable=[NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Contents/MacOS/Pro40DiagnosticWorker"].fileSystemRepresentation;
    launch.epoch=epoch;launch.errorFD=errors;
    #ifdef PRO40_ENABLE_DUPLEX_HARDWARE
    constexpr uint64_t guid=PRO40_MANAGED_REAL_GUID;
#else
    constexpr uint64_t guid=0x00130e0400000001;
#endif
    launch.specification=Pro40::WorkerLaunchSpecification::createDuplex(guid,log,{service,epoch,nonce,1,generation});
    for(char **item=environ;*item;++item)if(std::strncmp(*item,"PRO40_",6)!=0)launch.environment.emplace_back(*item);
    #ifdef PRO40_ENABLE_DUPLEX_FIXTURE
    launch.environment.push_back("PRO40_WORKER_TEST_LOCK_DIR="+leases);
#endif
    NSString *output=[NSString stringWithUTF8String:folder.c_str()];
    NSString *journal=[NSHomeDirectory() stringByAppendingPathComponent:@PRO40_LAB_JOURNAL];
    NSString *attempt=[[NSUUID.UUID.UUIDString lowercaseString] stringByReplacingOccurrencesOfString:@"-" withString:@""];
    const auto policy=nativeDuplexClockPolicy(tb.numer,tb.denom);
    const int result=runExperimentCommand([launch,listener=std::move(listener),policy,epoch,tb,output,journal,attempt,prestop,generation](dispatch_queue_t queue,auto report)mutable{
#ifdef PRO40_ENABLE_DUPLEX_HARDWARE
#ifdef PRO40_ENABLE_DUPLEX_LOOPBACK
        using HardwareRender=DuplexLoopbackRender;
#else
        using HardwareRender=DuplexHardwareRender;
#endif
        static_assert(uint64_t(HardwareRender::blocks)*HardwareRender::frames<
            uint64_t((Pro40::ManagedRunTiming::run-15s).count())*48000);
        auto hardware=std::make_shared<HardwareRender>();
        WorkerDuplexConnectionOwner::Render render=[hardware](HalDuplexMachSession &host,const std::atomic<bool> &cancel){
            return hardware->run(host,[&]{return cancel.load();},[]{return mach_absolute_time();},
                [](uint64_t target){return mach_wait_until(target)==KERN_SUCCESS;});
        };
        auto annotated=[report,output,hardware,generation](NSDictionary *value){
            // Pending supervision reports can precede audio-owner join. Do not
            // even read plain metrics/capture storage until the joined snapshot.
            NSMutableDictionary *m=[value mutableCopy];NSMutableArray *peaks=[NSMutableArray array],*nonzero=[NSMutableArray array];
            m[@"diagnostic_output"]=output;m[@"generated_duplex_only"]=@NO;m[@"hardware_duplex"]=@YES;
#ifdef PRO40_ENABLE_DUPLEX_LOOPBACK
            m[@"duplex_loopback"]=@YES;
#else
            m[@"duplex_loopback"]=@NO;
#endif
            if(![value[@"duplex_finished"] boolValue]){report(m);return;}
            for(unsigned c=0;c<8;++c){[peaks addObject:@(hardware->peak[c])];[nonzero addObject:@(hardware->nonzero[c])];}
            m[@"expected_generation"]=@(generation);m[@"duplex_test_blocks"]=@(hardware->completed);m[@"duplex_test_failure"]=@(hardware->failure);
            m[@"duplex_target_blocks"]=@(HardwareRender::blocks);m[@"duplex_block_frames"]=@(HardwareRender::frames);
            m[@"duplex_hardware_render"]=@{@"began":@(hardware->began),@"ended":@(hardware->ended),@"max_late_ticks":@(hardware->maxLateTicks),
                @"input_first":@(hardware->firstInput),@"output_first":@(hardware->firstOutput),@"output_sample_first":@(hardware->firstOutputSample),@"peaks":peaks,@"nonzero":nonzero};
#ifdef PRO40_ENABLE_DUPLEX_LOOPBACK
            if([value[@"supervision_finished"] boolValue] && hardware->completed==HardwareRender::blocks && !hardware->failure) {
                const auto analysis=LoopbackAnalysis::analyze(hardware->content,hardware->firstInput,hardware->firstOutputSample);
                NSMutableArray *matches=[NSMutableArray array];
                for(const auto &a:analysis.matches)[matches addObject:@{@"valid":@(a.valid),@"input_minus_output_sample_frames":@(a.inputMinusOutput),
                    @"callback_readback_frames":@(a.callbackReadback),@"recorded_index":@(a.recordedIndex),@"correlation":@(a.correlation),
                    @"competitor":@(a.competitor),@"peak":@(a.peak),@"rms":@(a.rms)}];
                m[@"loopback_analysis"]=@{@"valid":@(analysis.valid),@"minimum_frames":@(analysis.minimum),@"maximum_frames":@(analysis.maximum),
                    @"matches":matches,@"input_channel":@7,@"output_channel":@6,@"sample_rate":@48000,@"pcm_saved":@NO,
                    @"meaning":@"Electrical round-trip displacement on the current input/output sample mappings; not separate ADC/DAC latency or a HAL property value."};
            }
#endif
            report(m);
        };
#else
        auto progress=std::make_shared<DuplexDiagnosticProgress>();
        WorkerDuplexConnectionOwner::Render render=[progress](HalDuplexMachSession &host,const std::atomic<bool> &cancel){
                using Clock=std::chrono::steady_clock;
                AudioServerPlugInIOCycleInfo cycle{};std::array<float,137*8> input;std::array<float,137*6> output;
                const auto inputFirst=host.descriptor.inputFirst+37;
                progress->inputFirst.store(inputFirst);
                uint64_t written=host.descriptor.outputFirst+host.descriptor.primedOutputFrames;
                const auto began=Clock::now();auto due=began;
                const auto fail=[&](unsigned reason){progress->elapsedNS.store(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-began).count());progress->failure.store(reason);return false;};
                for(unsigned block=0;block<400;++block) {
                    progress->block.store(block);
                    if(cancel.load())return fail(1);
                    const auto first=inputFirst+block*137;
                    cycle.mInputTime.mSampleTime=double(first);cycle.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
                    if(!host.readInput(mach_absolute_time(),cycle,137,input))return fail(2);
                    for(unsigned f=0;f<137;++f)for(unsigned c=0;c<8;++c)
                        if(input[f*8+c]!=float(Fixture::sample(first+f,c))/8388608.f)return fail(3);
                    for(unsigned f=0;f<137;++f)for(unsigned c=0;c<6;++c)output[f*6+c]=float(Fixture::sample(written+f,c))/8388608.f;
                    cycle.mOutputTime.mSampleTime=double(written+host.descriptor.outputTimeline.callbackSample);cycle.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;
                    if(!host.writeOutput(mach_absolute_time(),cycle,137,output))return fail(4);
                    written+=137;due+=2854167ns;std::this_thread::sleep_until(due);
                }
                progress->block.store(400);
                progress->elapsedNS.store(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-began).count());
                SourceFrameClock::Stamp stamp;return host.timestamp(mach_absolute_time(),stamp) || fail(5);
            };
        auto annotated=[report,output,progress](NSDictionary *value){NSMutableDictionary *m=[value mutableCopy];m[@"diagnostic_output"]=output;m[@"generated_duplex_only"]=@YES;
            m[@"duplex_test_blocks"]=@(progress->block.load());m[@"duplex_test_failure"]=@(progress->failure.load());
            m[@"duplex_test_input_first"]=@(progress->inputFirst.load());m[@"duplex_test_elapsed_ns"]=@(progress->elapsedNS.load());report(m);};
#endif
        launch.duplex=std::make_shared<WorkerDuplexConnectionOwner>(queue,std::move(listener),SourceFrameClock::Identity{epoch,1,generation,tb.numer,tb.denom},policy,std::move(render));
        auto control=makeManagedTimedDiagnosticCommand(queue,journal,attempt,annotated,launch);
        if(prestop && control.stop)control.stop();return control;
    },attempt,journal,Pro40::ManagedRunTiming::command,@"managed-duplex-diagnostic");
    if(close(errors)!=0)return 74;return result;
#endif
}
}
