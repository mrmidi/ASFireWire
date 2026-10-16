// SPDX-License-Identifier: Apache-2.0
#include "../NativeSessionRunner.hpp"
#include "ParentWatch.hpp"
#include "DeviceLease.hpp"
#include "DeviceLeasePath.hpp"
#include "RecordingExport.hpp"
#include "TimingExport.hpp"
#include "ClockObservation.hpp"
#include "SixOutputCheck.hpp"
#ifdef PRO40_LIVE_PLAYBACK
#include "LivePlaybackCheck.hpp"
#endif
#include "../InputMonitor.hpp"
#ifdef PRO40_SUSTAINED_INPUT
#include "../SustainedInputBridge.hpp"
#endif
#include <mach/mach_time.h>
#include <IOKit/IOReturn.h>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#ifdef PRO40_MANAGED_WORKER
#ifdef PRO40_WORKER_ADMISSION_TEST
#error Managed workers cannot use the legacy test admission hook
#endif
#include "ManagedWorkerAdmission.hpp"
#include "WorkerDiagnosticTiming.hpp"
#endif
#ifdef PRO40_DUPLEX_WORKER
#if !defined(PRO40_MANAGED_WORKER) || !defined(PRO40_LIVE_PLAYBACK) || !defined(PRO40_SUSTAINED_INPUT) || !defined(PRO40_DMA_SUSTAINED)
#error Duplex workers require managed admission and the sustained live profile
#endif
#include "DuplexWorkerRun.hpp"
#endif
#ifdef PRO40_WORKER_ADMISSION_TEST
#ifndef PRO40_WORKER_TEST_LOCKS
#error Admission test hook requires hardware-free test leases
#endif
namespace Pro40 {std::optional<NativeSessionResult> testWorkerAdmission(StopQuery);}
#endif
namespace {
volatile std::sig_atomic_t stopped=0;
void stop(int){stopped=1;}
bool requested(void *context) noexcept {
    return stopped!=0 || static_cast<Pro40::ParentWatch *>(context)->requested();
}
[[maybe_unused]] Pro40::NativeSessionResult run(int argc,char **argv,Pro40::ParentWatch &parent,Pro40::DeviceLease &lease) {
    const bool sustained=argc==6 && std::string(argv[1])=="--sustain-check";
#ifdef PRO40_DMA_SUSTAINED
    if(!sustained)return {64,{}};
#else
    if(sustained)return {64,{}};
#endif
    unsigned sustainSeconds=0;
    if(sustained) {
        const std::string seconds=argv[2];
        const auto result=std::from_chars(seconds.data(),seconds.data()+seconds.size(),sustainSeconds);
        if(result.ec!=std::errc{} || result.ptr!=seconds.data()+seconds.size() || !sustainSeconds || sustainSeconds>3600)
            return {64,{}};
        argc-=2;argv+=2;
    }
    const bool eightSix=argc==5 && std::string(argv[1])=="--check-eight-six";
#if !defined(PRO40_EIGHT_SIX_CHECK) || !defined(PRO40_DMA_CYCLE_BUDGET) || PRO40_DMA_CYCLE_BUDGET != 262144
    if(eightSix)return {64,{}};
#endif
    const bool recordEight=eightSix || (argc==5 && std::string(argv[1])=="--record-eight");
    const bool warmup=argc==5 && std::string(argv[1])=="--warm-up";
    const bool quietControl=argc==5 && std::string(argv[1])=="--record-quiet-control";
#ifdef PRO40_STARTUP_QUIET_CONTROL
    if(!quietControl)return {64,{}};
#else
    if(quietControl)return {64,{}};
#endif
    const bool startupRecovery=argc==5 && std::string(argv[1])=="--record-startup-recovery";
    const bool clockExcursion=argc==5 && std::string(argv[1])=="--record-clock-excursion";
    const bool enableFirst=quietControl || warmup || startupRecovery || clockExcursion || (argc==5 && std::string(argv[1])=="--record-enable-first");
    const bool existingClock=argc==5 && std::string(argv[1])=="--record-existing-clock";
    const bool assignedIdle=enableFirst || existingClock || (argc==5 && std::string(argv[1])=="--record-assigned-idle");
    const bool ownedIdle=assignedIdle || (argc==5 && std::string(argv[1])=="--record-owned-idle");
    const bool quietHold=ownedIdle || (argc==5 && std::string(argv[1])=="--record-quiet-hold");
    const bool postRelease=quietHold || (argc==5 && std::string(argv[1])=="--record-post-release");
    const bool timed=!warmup && (postRelease || (argc==5 && std::string(argv[1])=="--record-timing"));
    const bool record=recordEight || timed || (argc==5 && std::string(argv[1])=="--record-input");
    if(record || warmup){--argc;++argv;}
    if(argc!=4 || std::string(argv[1])!="--guid")return {64,{}};
    Pro40::NativeSessionOptions options;options.postReleasePeaks=postRelease;options.preCloseQuietHold=quietHold;options.ownedIdleObservation=ownedIdle;options.assignedIdleObservation=assignedIdle;options.verifyExistingClockOnly=existingClock;options.enableFirstStartup=enableFirst;options.clockExcursion=clockExcursion;
    options.warmupOnly=warmup;
    options.startupRecovery=startupRecovery;
    options.quietStartupControl=quietControl;
    Pro40Packets::InputMonitor inputMonitor;
    options.sustained=sustained;options.stopAfterSeconds=sustainSeconds;
    if(sustained)options.inputMonitor=&inputMonitor;
    const std::string input=argv[2];
    const auto parsed=std::from_chars(input.data(),input.data()+input.size(),options.guid,16);
    if(parsed.ec!=std::errc{} || parsed.ptr!=input.data()+input.size())return {64,{}};
    options.logPath=argv[3];options.retainHistory=false;options.stop={requested,&parent};
    std::unique_ptr<Pro40Packets::InputRecording> recording;
    if(record) {
        recording.reset(new(std::nothrow) Pro40Packets::InputRecording(recordEight?8:1));
        if(!recording || !recording->valid())return {1,{}};
        options.recording=recording.get();
    }
#ifndef PRO40_LIVE_PLAYBACK
    std::unique_ptr<Pro40::SixOutputCheck> outputCheck;
    Pro40Packets::PlaybackClip clip;
    if(eightSix || sustained) {
        outputCheck.reset(new(std::nothrow) Pro40::SixOutputCheck);
        if(!outputCheck || !outputCheck->valid())return {1,{}};
        clip=outputCheck->clip();options.playback=&clip;
    }
#endif
#ifdef PRO40_LIVE_PLAYBACK
    auto live=std::unique_ptr<Pro40::LivePlaybackCheck>(new(std::nothrow) Pro40::LivePlaybackCheck);
    if(!live || !live->prepare())return {1,{}};
    options.livePlayback=&live->source;
#endif
    std::unique_ptr<Pro40::NativeTimingEvidence> timing;
    std::optional<Pro40::InputSessionObservation> inputObservation;
    std::optional<Pro40::ClockObservation> clockObservation;
    if(timed) {
        timing.reset(new(std::nothrow) Pro40::NativeTimingEvidence);
        mach_timebase_info_data_t clock{};
        if(!timing || !timing->packets.valid() || mach_timebase_info(&clock)!=KERN_SUCCESS)return {1,{}};
        timing->numer=clock.numer;timing->denom=clock.denom;options.timing=timing.get();
        inputObservation.emplace(*recording,clock.numer,clock.denom);
        clockObservation.emplace(*timing,recording.get(),&*inputObservation);
    }
#ifdef PRO40_SUSTAINED_INPUT
    mach_timebase_info_data_t bridgeTimebase{};
    if(mach_timebase_info(&bridgeTimebase)!=KERN_SUCCESS)return {1,{}};
    auto bridge=std::unique_ptr<Pro40Audio::SustainedInputBridge>(new(std::nothrow)
        Pro40Audio::SustainedInputBridge(bridgeTimebase.numer,bridgeTimebase.denom,1));
    if(!bridge)return {1,{}};
    options.inputHandoff=&bridge->handoff;
#endif
    const auto directory=Pro40::deviceLeaseDirectory();
    if(!directory)return {1,{}};
    const auto admission=lease.acquire(directory->c_str(),options.guid);
    if(admission!=Pro40::DeviceAdmission::acquired) {
        std::fprintf(stderr,"worker-device-admission: %s\n",admission==Pro40::DeviceAdmission::busy?"busy":
            admission==Pro40::DeviceAdmission::unclean?"unclean":"io-error");
        return {admission==Pro40::DeviceAdmission::ioError?1:75,{}};
    }
#ifdef PRO40_LIVE_PLAYBACK
    if(!live->start()) {
        if(!lease.confirmClean()){Pro40::SessionOutcome failed;failed.cleanup=kIOReturnIOError;return {2,failed};}
        return {1,{}};
    }
#endif
#ifdef PRO40_SUSTAINED_INPUT
    if(!bridge->start()) {
        // No native entry, so the lease can be released cleanly.
        if(!lease.confirmClean()) {
            Pro40::SessionOutcome failed;failed.cleanup=kIOReturnIOError;return {2,failed};
        }
        return {1,{}};
    }
#endif
    auto result=Pro40::runNativeSession(options);
#ifdef PRO40_LIVE_PLAYBACK
    const bool playbackOK=live->finish();
    char playbackReport[768];
    const auto lowWater=live->queue.lowWater();
    const int playbackSize=std::snprintf(playbackReport,sizeof(playbackReport),
        "{\"schema\":1,\"source_ok\":%s,\"fault\":%u,\"written_frames\":%llu,\"consumed_frames\":%llu,"
        "\"callbacks\":%llu,\"queue_high_water\":%llu,\"queue_low_water\":%llu,\"clamped_samples\":%llu,"
        "\"application_connected\":false}\n",
        playbackOK?"true":"false",unsigned(live->queue.fault()),
        (unsigned long long)live->queue.written(),(unsigned long long)live->queue.consumed(),
        (unsigned long long)live->callbacks(),(unsigned long long)live->queue.highWater(),
        (unsigned long long)(lowWater==UINT64_MAX?0:lowWater),(unsigned long long)live->queue.clamped());
    const bool playbackSaved=playbackSize>0 && size_t(playbackSize)<sizeof(playbackReport) &&
        Pro40::publishRecordingFile(options.logPath+".live-playback.json",
            {reinterpret_cast<const uint8_t *>(playbackReport),size_t(playbackSize)});
    if(result.accepted() && (!playbackOK || !playbackSaved)) {result.outcome->evidence=kIOReturnError;result.exitCode=2;}
    std::fprintf(stderr,"live-playback: %s report=%s\n",playbackOK?"ok":"failed",playbackSaved?"saved":"failed");
#endif
#ifdef PRO40_SUSTAINED_INPUT
    const bool bridgeOK=bridge->finish(result.accepted());
    const auto &reader=bridge->receiver;
    Pro40Audio::NativeClockSnapshot clockSnapshot;
    const bool haveSnapshot=reader.snapshot(clockSnapshot);
    char bridgeReport[1600];
    const int reportSize=std::snprintf(bridgeReport,sizeof(bridgeReport),
        "{\"schema\":1,\"accepted\":%s,\"fault\":%u,\"packets\":%llu,\"frames\":%llu,"
        "\"published_frames\":%llu,\"queue_high_water\":%llu,\"clock_pairs\":%llu,"
        "\"snapshots\":%llu,\"snapshot_frames\":%llu,\"have_snapshot\":%s,"
        "\"packet_fit_count\":%llu,\"bus_fit_count\":%llu,\"syt_ticks_per_frame\":%.12Lg,"
        "\"bus_seconds_per_host_second\":%.12Lg,\"max_packet_age_ticks\":%llu,"
        "\"hal_clock_admitted\":false}\n",
        bridgeOK?"true":"false",unsigned(bridge->handoff.fault()),
        (unsigned long long)reader.packets(),(unsigned long long)reader.frames(),
        (unsigned long long)reader.publishedNext(),(unsigned long long)bridge->handoff.highWater(),
        (unsigned long long)reader.clockPairs(),(unsigned long long)reader.snapshots(),
        (unsigned long long)reader.snapshotFrames(),haveSnapshot?"true":"false",
        (unsigned long long)clockSnapshot.packet.count,(unsigned long long)clockSnapshot.bus.count,
        clockSnapshot.packet.slope,clockSnapshot.bus.slope,(unsigned long long)clockSnapshot.maxPacketAgeTicks);
    const bool reportOK=reportSize>0 && size_t(reportSize)<sizeof(bridgeReport) &&
        Pro40::publishRecordingFile(options.logPath+".sustained-input.json",
            {reinterpret_cast<const uint8_t *>(bridgeReport),size_t(reportSize)});
    if(result.accepted() && (!bridgeOK || !reportOK)) {
        result.outcome->evidence=kIOReturnError;result.exitCode=2;
    }
    std::fprintf(stderr,"sustained-input-bridge: %s report=%s\n",bridgeOK?"accepted":"failed",reportOK?"saved":"failed");
#endif
    if(inputObservation)inputObservation->finish(mach_absolute_time(),result.accepted());
    const bool clean=result.outcome?result.outcome->cleanup==0 && (result.exitCode==0 || result.exitCode==2):
        (result.exitCode==1 || result.exitCode==64 || result.exitCode==75);
    if(clean && !lease.confirmClean()) {
        std::fprintf(stderr,"worker-device-admission: clean-record-failed\n");
        if(!result.outcome)result.outcome=Pro40::SessionOutcome{};
        if(!result.outcome->cleanup)result.outcome->cleanup=kIOReturnIOError;
        result.exitCode=2;
    }
    // File failures do not change native cleanup or the device lease state.
    // The separate report is required before the UI may claim audio was saved.
    if(recording)Pro40::exportRecording(options.logPath,*recording,result);
    if(timing)Pro40::exportTiming(options.logPath,*timing,*recording,result);
    if(clockObservation)std::fprintf(stderr,"clock-observer-report: %s\n",
        clockObservation->exportReport(options.logPath,result)?"saved":"failed");
    if(clockObservation)std::fprintf(stderr,"audio-startup-report: %s\n",
        clockObservation->exportStartupReport(options.logPath,result)?"saved":"failed");
    if(inputObservation)std::fprintf(stderr,"input-session-report: %s\n",
        inputObservation->exportReport(options.logPath,result)?"saved":"failed");
    return result;
}
} // namespace
int main(int argc,char **argv) {
    // Keep a private protocol pipe before redirecting all C/native stdout to
    // stderr. Never let native event text enter the supervisor's JSON stream.
    const int fd=dup(STDOUT_FILENO);
    if(fd<0)return 74;
    if(fcntl(fd,F_SETFD,FD_CLOEXEC)<0 || dup2(STDERR_FILENO,STDOUT_FILENO)<0) {
        close(fd);return 74;
    }
    FILE *protocol=fdopen(fd,"w");
    if(!protocol){close(fd);return 74;}
    if(std::signal(SIGINT,stop)==SIG_ERR || std::signal(SIGTERM,stop)==SIG_ERR ||
       std::signal(SIGPIPE,SIG_IGN)==SIG_ERR) {std::fclose(protocol);return 74;}
    Pro40::DeviceLease lease; // Held through result IO and monitor destruction.
    Pro40::ParentWatch parent;
    if(!parent.start(STDIN_FILENO)){std::fclose(protocol);return 74;}
    if(std::fprintf(protocol,"{\"version\":1,\"type\":\"ready\"}\n")<0 || std::fflush(protocol)) {
        std::fclose(protocol);return 74; // No native entry if ready cannot be sent.
    }
    const auto result=[&]{
#ifdef PRO40_MANAGED_WORKER
#ifdef PRO40_DUPLEX_WORKER
        Pro40::ManagedWorkerPeer peer;
#endif
        // Mandatory for this executable, including invocations without the flag.
        // No fallback to standalone entry after absent or failed admission.
        if(auto refused=Pro40::managedWorkerAdmission(argc,argv,{requested,&parent},
            std::chrono::steady_clock::now()+Pro40::WorkerDiagnosticTiming::admission,
#ifdef PRO40_DUPLEX_WORKER
            &peer
#else
            nullptr
#endif
            ))return *refused;
#endif
#ifdef PRO40_WORKER_ADMISSION_TEST
        // Test-only integration seam; no environment-controlled release in real
        // workers. The test gate runs before recording allocation or lease IO.
        if(auto refused=Pro40::testWorkerAdmission({requested,&parent}))return *refused;
#endif
#ifdef PRO40_DUPLEX_WORKER
        return Pro40::runDuplexWorker(argc,argv,{requested,&parent},lease,peer);
#else
        return run(argc,argv,parent,lease); // May _Exit(3); leaves active record.
#endif
    }();
    if(parent.requested())std::fprintf(stderr,"worker-control-stop: 1\n");
    int written=0;
    if(result.outcome) {
        const auto &r=*result.outcome;
        written=std::fprintf(protocol,"{\"version\":1,\"type\":\"result\",\"exit\":%d,\"outcome\":{\"reason\":\"%s\",\"transfer\":%d,\"evidence\":%d,\"cleanup\":%d}}\n",
            result.exitCode,Pro40::name(r.reason),int(r.transfer),int(r.evidence),int(r.cleanup));
    } else written=std::fprintf(protocol,"{\"version\":1,\"type\":\"result\",\"exit\":%d,\"outcome\":null}\n",result.exitCode);
    const bool failed=written<0 || std::fflush(protocol)!=0;
    const bool closeFailed=std::fclose(protocol)!=0;
    return failed || closeFailed?74:result.exitCode;
}
