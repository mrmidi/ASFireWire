// SPDX-License-Identifier: Apache-2.0
// Bounded startup observation: real device TX capture + host no-data TX.
// DUPLEX_REPLAY substitutes a finite measured-cadence silence tail using distant
// descriptor updates; selected profiles reuse bounded banks. An explicit clip
// can supply PCM on playback stream A. No CoreAudio integration yet.
#include "Pro40ConfigBackend.hpp"
#include "Pro40ChannelConfig.hpp"
#include "Pro40StreamEnable.hpp"
#include "GenerationReservation.hpp"
#include "DmaPreparation.hpp"
#include "DiagnosticRecords.hpp"
#include "MeterRouteSnapshot.hpp"
#include "InputBoundaryTrace.hpp"
#include "PostReleasePeakWindow.hpp"
#include "PreCloseQuietHold.hpp"
#include "StagedMeterRoutes.hpp"
#include "TransmitMarginSummary.hpp"
#include "MeasuredDiagnostic.hpp"
#include "NativeSessionRunner.hpp"
#include "InputRecording.hpp"
#include "PlaybackClip.hpp"
#include "PlaybackPcmSource.hpp"
#ifdef PRO40_LIVE_PLAYBACK
#if !defined(PRO40_DMA_SUSTAINED) || !defined(PRO40_SUSTAINED_INPUT)
#error Live playback requires the sustained input/clock profile
#endif
#endif
#include "InputMonitor.hpp"
#include "SustainedSession.hpp"
#include "SustainedInputHandoff.hpp"
#include "NativeInputDelivery.hpp"
#include "NativeStopDrain.hpp"
#ifdef PRO40_DUPLEX_WORKER
#include "NativeAudioScheduling.hpp"
#endif
#ifdef PRO40_SUSTAINED_INPUT
#ifndef PRO40_DMA_SUSTAINED
#error Sustained input handoff requires the sustained transport profile
#endif
#endif
#include "NativeTimingEvidence.hpp"
#include "NativeClockEvidenceAge.hpp"
#include <mach/mach_time.h>
#include "SessionAdmission.hpp"
#include "StartupGate.hpp"
#include "EnableFirstStartup.hpp"
#include "ClockExcursion.hpp"
#include "StartupRecovery.hpp"
#ifdef PRO40_DMA_TX_LOCK
#ifndef PRO40_DMA_TX_LONG_REFILL
#error Playback lock acceptance requires the bounded long-refill profile.
#endif
#include "PlaybackLockCheck.hpp"
#include "StagedLockPoll.hpp"
#include "NativeAsyncStatusRead.hpp"
#include "QuietStartupControl.hpp"
#endif
#ifdef PRO40_STARTUP_QUIET_CONTROL
#if !defined(PRO40_STARTUP_TIMELINE) || !defined(PRO40_DMA_TX_LOCK) || !defined(PRO40_DMA_SOURCE_QUEUE) || PRO40_DMA_CYCLE_BUDGET != 262144 || defined(PRO40_STARTUP_RECOVERY)
#error Quiet control requires the isolated 262144-cycle timeline profile without automatic recovery.
#endif
#endif
#ifdef PRO40_DMA_TX_MEASURED
#include "Pro40NativeReplay.hpp"
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
#include "Pro40RollingReplay.hpp"
#include <vector>
#endif
#include <string>
#include <memory>
#include <new>
#include <mach/mach.h>
#include <time.h>
#ifdef PRO40_DUPLEX_WORKER
#include "RunLoopDispatchObservation.hpp"
#endif

namespace {
#ifdef PRO40_DMA_DUPLEX_REPLAY
static constexpr unsigned txCount=DmaPreparation::replayTransmitCount;
static constexpr unsigned rxCount=DmaPreparation::descriptorCount;
#elif defined(PRO40_DMA_TX_RETURN)
static constexpr unsigned txCount=DmaPreparation::transmitReturnCount,rxCount=DmaPreparation::descriptorCount;
#else
static constexpr unsigned txCount=512;
#endif
static constexpr unsigned initialLead=16000;

// Diagnostic timing only. No logging or allocation while the transfer runs.
struct ProbeCost {
    unsigned calls=0;
    uint64_t total=0,maximum=0;
    template<class Work> auto measure(Work work) {
        const auto begin=std::chrono::steady_clock::now();
        const auto result=work();
        const auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-begin).count());
        ++calls;total+=elapsed;maximum=std::max(maximum,elapsed);
        return result;
    }
    void emit(FILE *log,const char *name) const {
        std::fprintf(log,"duplex-cost stage=%s calls=%u total-us=%llu max-us=%llu\n",name,calls,
            static_cast<unsigned long long>(total),static_cast<unsigned long long>(maximum));
    }
};
struct CallbackQueueState {
    mach_port_status_t info{};
    kern_return_t result=KERN_FAILURE;
    void read(mach_port_t port) {
        mach_msg_type_number_t count=MACH_PORT_RECEIVE_STATUS_COUNT;
        result=mach_port_get_attributes(mach_task_self(),port,MACH_PORT_RECEIVE_STATUS,
            reinterpret_cast<mach_port_info_t>(&info),&count);
    }
};
static Pro40::SessionAdmission runnerAdmission;
void meterRoutes(Pro40OwnedSession &session,const char *stage) {
    Pro40::MeterRouteSnapshot snapshot;
    snapshot.capture(session.global,[&](uint64_t address,unsigned size,uint8_t *bytes) {
        return pro40_read(session.generation,address,size,bytes);
    },[&] {return session.stable();});
    std::fprintf(session.log,"meter-routes stage=%s valid=%u io=0x%08x error=%u count=%u enable-before=%u enable-after=%u words=",
        stage,unsigned(snapshot.valid),unsigned(snapshot.ioStatus),snapshot.error,snapshot.count,
        snapshot.enableBefore,snapshot.enableAfter);
    for(unsigned i=0;i<snapshot.words.size();++i)
        std::fprintf(session.log,"%s%08x",i?",":"",snapshot.words[i]);
    std::fprintf(session.log,"\n");
}
int32_t startupStatus(bool stop,uint32_t lead,uint32_t minimum,uint32_t maximum,bool expired=false) {
    switch(Pro40::startupDecision(stop,lead,minimum,maximum,expired)) {
    case Pro40::StartupDecision::interrupted:return kIOReturnAborted;
    case Pro40::StartupDecision::timedOut:return kIOReturnTimeout;
    default:return 0;
    }
}
struct EnableBackend {
    ConfigBackend &config;
    uint32_t target;
    Pro40::StopQuery stop;
    uint32_t enableLead=0;
    bool enableLeadObserved=false;
    uint32_t enableMinimum=128,enableMaximum=512;
    int32_t streamGuard() {return config.session.streamGuard();}
    int32_t ownerGuard() {
        auto &s=config.session;
        auto result=s.stable();
        // Shutdown rechecks the actual owner before clearing ENABLE. Live
        // refills instead use streamGuard and the verified generation lease.
        // A lost clock must not prevent disabling our owned device.
        // Linux dice-interface.h:49-57 defines the full eight-byte GLOBAL_OWNER.
        std::array<uint8_t,8> bytes{};
        if(!result)result=pro40_read(s.generation,s.global,8,bytes.data());
        if(!result && ((uint64_t(ownedBe32(bytes.data()))<<32)|ownedBe32(bytes.data()+4))!=s.owner)
            result=kIOReturnNotPermitted;
        return result;
    }
    int32_t lead(uint32_t &cycles) {
        UInt32 timer=0;
        auto &s=config.session;
        auto result=(*s.device)->GetCycleTime(s.device,&timer);
        const auto cycle=(timer>>12)&0x1fff;
        if(!result && cycle>=8000)result=kIOReturnError;
        cycles=(target+32000-(((timer>>25)&3)*8000+cycle))%32000;
        return result;
    }
    int32_t enableGuard() {
        auto result=config.guard();uint32_t cycles=0;
        if(!result){result=lead(cycles);if(!result){enableLead=cycles;enableLeadObserved=true;}}
        // Normal starts are armed; diagnostic version 2 enables before host starts.
        if(!result)result=startupStatus(stop.requested(),cycles,enableMinimum,enableMaximum);
        return result;
    }
    int32_t writeEnabled(uint32_t value) {
        return config.write(config.session.global+0x50,value);
    }
    int32_t readEnabled(uint32_t &value) {
        return config.read(config.session.global+0x50,value);
    }
};
} // namespace
Pro40::NativeSessionResult Pro40::runNativeSession(const NativeSessionOptions &requestedOptions) {
    auto options=requestedOptions;
    if(options.logPath.empty())return {64,{}};
#ifdef PRO40_DMA_SUSTAINED
    if(!options.sustained)return {64,{}};
#else
    if(options.sustained)return {64,{}};
#endif
    if(options.inputHandoff && (!options.sustained || !options.inputHandoff->fresh()))return {64,{}};
#ifndef PRO40_SUSTAINED_INPUT
    if(options.inputHandoff)return {64,{}};
#endif
    if(options.livePlayback && (!options.sustained || !options.inputHandoff || options.playback ||
        !options.livePlayback->valid() || options.livePlayback->channels!=6))return {64,{}};
#ifndef PRO40_LIVE_PLAYBACK
    if(options.livePlayback)return {64,{}};
#endif
    if(options.sustained) {
        if(options.retainHistory || options.recording || options.timing ||
            !options.inputMonitor || options.inputMonitor->fault || options.inputMonitor->packets || options.inputMonitor->frames ||
            options.stopAfterSeconds>3600 || options.warmupOnly || options.startupRecovery ||
            options.quietStartupControl || options.postReleasePeaks || options.preCloseQuietHold ||
            options.ownedIdleObservation || options.assignedIdleObservation || options.enableFirstStartup ||
            options.verifyExistingClockOnly || options.clockExcursion || options.peerPreparation.prepare ||
            options.peerCleanup.finish || options.sourceCompletion.finish)return {64,{}};
    } else if(options.stopAfterSeconds || options.inputMonitor)return {64,{}};
    // Timing/feed consumers currently decode mono storage. Eight-channel
    // recording is admitted only without those consumers, never mis-timestamped.
    if(options.recording && (!options.recording->valid() || options.recording->fault() ||
        (options.timing && options.recording->channels()!=1)))return {64,{}};
    if(options.playback && (!options.playback->valid() || options.warmupOnly ||
        options.startupRecovery || options.quietStartupControl || options.postReleasePeaks ||
        options.peerPreparation.prepare || options.peerCleanup.finish || options.sourceCompletion.finish))return {64,{}};
#if !defined(PRO40_DMA_SOURCE_QUEUE) || !defined(PRO40_DMA_TX_MEASURED)
    if(options.playback)return {64,{}};
#endif
#ifdef PRO40_STARTUP_QUIET_CONTROL
    if(!options.quietStartupControl || options.startupRecovery || options.warmupOnly ||
        options.retainHistory || !options.recording || !options.timing || options.timing->hasConsumer() ||
        !options.enableFirstStartup || !options.assignedIdleObservation || options.clockExcursion ||
        options.verifyExistingClockOnly || options.peerPreparation.prepare ||
        options.peerCleanup.finish || options.sourceCompletion.finish)return {64,{}};
#else
    if(options.quietStartupControl)return {64,{}};
#endif
    if(options.startupRecovery && (options.warmupOnly || options.retainHistory ||
        !options.recording || !options.timing || options.timing->hasConsumer() ||
        !options.enableFirstStartup || !options.assignedIdleObservation ||
        options.clockExcursion || options.verifyExistingClockOnly ||
        options.peerPreparation.prepare || options.peerCleanup.finish || options.sourceCompletion.finish))return {64,{}};
#ifndef PRO40_STARTUP_RECOVERY
    if(options.startupRecovery)return {64,{}};
#endif
    if(options.warmupOnly && (options.retainHistory || options.recording || options.timing ||
        !options.enableFirstStartup || !options.assignedIdleObservation || options.clockExcursion ||
        options.verifyExistingClockOnly || options.peerPreparation.prepare ||
        options.peerCleanup.finish || options.sourceCompletion.finish))return {64,{}};
#if !defined(PRO40_DMA_SOURCE_QUEUE) || !defined(PRO40_DMA_TX_MEASURED) || !defined(PRO40_DMA_CYCLE_BUDGET) || PRO40_DMA_CYCLE_BUDGET != 65536
    if(options.warmupOnly)return {64,{}};
#endif
    if(options.peerPreparation.prepare&&!options.peerCleanup.finish)return {64,{}};
    if(options.sourceCompletion.finish&&!options.peerCleanup.finish)return {64,{}};
    if(options.timing&&options.timing->hasConsumer()&&!options.recording)return {64,{}};
    if(options.postReleasePeaks && ((!options.warmupOnly && (!options.recording || !options.timing)) ||
        (options.timing && options.timing->hasConsumer()) ||
        options.peerPreparation.prepare || options.peerCleanup.finish || options.sourceCompletion.finish))return {64,{}};
    if(options.preCloseQuietHold && !options.postReleasePeaks)return {64,{}};
    if(options.ownedIdleObservation && !options.preCloseQuietHold)return {64,{}};
    if(options.assignedIdleObservation && !options.ownedIdleObservation)return {64,{}};
    if(options.clockExcursion && (!options.enableFirstStartup || !options.assignedIdleObservation || options.verifyExistingClockOnly))return {64,{}};
    if(options.verifyExistingClockOnly && !options.assignedIdleObservation)return {64,{}};
    if(options.enableFirstStartup && (!options.assignedIdleObservation || options.verifyExistingClockOnly))return {64,{}};
    const bool retainHistory=options.retainHistory;
#ifdef PRO40_DMA_SOURCE_QUEUE
    if(txCount>8192 && retainHistory)return {64,{}};
#else
    if(!retainHistory)return {64,{}};
#endif
    SessionAdmission::Lease admission(runnerAdmission);
    if(!admission)return {75,{}};
    const auto guid=options.guid;
    const auto &stop=options.stop;
    // An admitted cancellation needs no native setup, storage or log files.
    // Cleanup/evidence are zero because nothing was acquired or transferred;
    // interrupted reason still prevents acceptance as a completed diagnostic.
    if(stop.requested())return {2,SessionOutcome{StopReason::interrupted,kIOReturnAborted,0,0}};
#ifdef PRO40_DUPLEX_WORKER
    // Allocate and attach before native entry. This observation remains disarmed
    // during setup/stop and is retired on this thread after native close.
    RunLoopDispatchObservation dispatchObserver;
    if(!dispatchObserver.valid())return {1,{}};
    NativeAudioScheduling nativeScheduling;
    if(!nativeScheduling.prepared())return {1,{}};
#endif
    // Large finite evidence storage must not consume the main thread stack.
    // Allocate before opening or owning hardware; retain through native close.
    using DmaStorage=std::array<DmaPreparation,4>;
    auto dmaStorage=std::unique_ptr<DmaStorage>(new(std::nothrow) DmaStorage);
    std::unique_ptr<DmaStorage> retiredDmaStorage;
    if(!dmaStorage)return {1,{}};
#ifdef PRO40_DMA_TX_REFRESH
    constexpr unsigned firstRefreshCycle=DmaPreparation::firstRefreshCycle;
    constexpr unsigned transmitBankPackets=DmaPreparation::transmitBankPackets;
    constexpr unsigned transmitSlots=DmaPreparation::transmitSlots;
    constexpr unsigned firstRefreshCount=3*transmitBankPackets-firstRefreshCycle;
    constexpr unsigned refreshCount=txCount-firstRefreshCycle;
#ifdef PRO40_DMA_SOURCE_QUEUE
    constexpr unsigned refreshSlots=transmitBankPackets;
    static_assert(firstRefreshCycle==transmitSlots && firstRefreshCount==transmitBankPackets);
#else
    constexpr unsigned refreshSlots=refreshCount;
#endif
    using RefreshStorage=std::array<std::array<NativePackets::PreparedBuffer,refreshSlots>,2>;
    auto refreshedStorage=std::unique_ptr<RefreshStorage>(new(std::nothrow) RefreshStorage);
    if(!refreshedStorage)return {1,{}};
#endif
    FILE *log=std::fopen(options.logPath.c_str(),"wx");if(!log)return {1,{}};
#if defined(PRO40_DMA_DUPLEX_REPLAY) && defined(PRO40_DMA_RX_REUSE)
    std::array<FILE *,6> captures{};
#elif defined(PRO40_DMA_DUPLEX_REPLAY) || defined(PRO40_DMA_RX_REUSE)
    std::array<FILE *,4> captures{};
#else
    std::array<FILE *,2> captures{};
#endif
    for(unsigned i=0;retainHistory && i<captures.size();++i) {
#if defined(PRO40_DMA_RX_REUSE) && defined(PRO40_DMA_DUPLEX_REPLAY)
        const auto path=options.logPath+(i<2?".rx":i<4?".tx":".rx")+std::to_string(i%2)+(i<4?".bin":".final.bin");
#elif defined(PRO40_DMA_RX_REUSE)
        const auto path=options.logPath+".rx"+std::to_string(i%2)+(i<2?".bin":".final.bin");
#else
        const auto path=options.logPath+(i<2?".rx":".tx")+std::to_string(i%2)+".bin";
#endif
        captures[i]=std::fopen(path.c_str(),"wbx");
        if(!captures[i]) {for(auto f:captures)if(f)std::fclose(f);std::fclose(log);return {1,{}};}
    }
    Pro40::SessionLifecycle lifecycle;
    (void)lifecycle.start(); // Fresh control object, before any native ownership.
    Pro40OwnedSession session{};session.log=log;session.quietNotifications=true;
    if(options.warmupOnly)std::fprintf(log,"warmup-policy version=1 cycles=65536 input=discarded retry=none\n");
    Pro40::InputBoundaryTrace inputBoundaries;
    auto inputGuard=[&](uint64_t expectedOwner)->int32_t{
        auto e=session.stable();std::array<uint8_t,8> bytes{};
        if(!e)e=pro40_read(session.generation,session.global,8,bytes.data());
        if(!e&&((uint64_t(ownedBe32(bytes.data()))<<32)|ownedBe32(bytes.data()+4))!=expectedOwner)e=kIOReturnNotPermitted;
        if(!e)e=pro40_read(session.generation,session.global+0x50,4,bytes.data());
        if(!e&&(ownedBe32(bytes.data())!=0||session.invalidPacket))e=kIOReturnNotPermitted;
        if(!e)e=session.stable();return e;
    };
    auto inputRead=[&](auto address,auto count,auto bytes){return pro40_read(session.generation,address,count,bytes);};
    auto inputBoundary=[&](Pro40::InputBoundaryTrace::Stage stage,uint64_t expectedOwner){
        inputBoundaries.capture(stage,inputRead,[&]{return inputGuard(expectedOwner);},[&]{return session.startupTrace.now();});
    };
    Pro40::PostReleasePeakWindow postRelease;
    bool postReleaseStarted=false;uint64_t postReleaseOrigin=0;
    bool cleanupOriginSet=false,quietStarted=false;
    uint64_t cleanupOrigin=0,quietBegin=0,quietEnd=0;
    auto quietState=PreCloseQuietHold::State::insufficientBudget;
    session.startupTrace.start();
    auto status=pro40_open(guid);session.event("duplex-open",unsigned(status));
    if(!status&&options.peerPreparation.prepare) {
        if(stop.requested())status=kIOReturnAborted;
        else status=options.peerPreparation.status();
        if(!status&&stop.requested())status=kIOReturnAborted;
        session.event("duplex-peer-prepared",unsigned(status));
    }
    if(!status)status=session.setup(options.expectedGeneration);
    session.event("duplex-owner",unsigned(status));
    // Linux dice-stream.c:289-301 always reselects after owning the device,
    // even at the current rate, to refresh potentially invalid stream state.
    if(!status && !stop.requested())meterRoutes(session,"before-clock");
    if(!status && !stop.requested())inputBoundary(Pro40::InputBoundaryTrace::Stage::beforeClock,session.owner);
    session.startupTrace.phase(Pro40::StartupNotificationTrace::Phase::clock);
    if(!status && !stop.requested()) {
        if(options.clockExcursion) {
            session.requireRestoredClock=true;
            session.event("diagnostic-clock-excursion",1);
            struct ExcursionBackend {
                Pro40OwnedSession &session;
                Pro40::StopQuery stop;
                uint64_t nowMicros(){return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());}
                bool cancelled(){return stop.requested();}
                int32_t guard(){return session.stable();}
                int32_t read(Pro40::ExistingClockSample &sample){
                    Pro40ClockState state{};const auto result=session.state(state);
                    if(!result) {
                        sample={session.ownerOf(state),state.enabled,state.selected,state.status,state.measured,state.capabilities};
                        std::fprintf(session.log,"clock-excursion-readback selected=%08x measured=%u status=%08x enabled=%u capabilities=%08x owner=%016llx\n",
                            state.selected,state.measured,state.status,state.enabled,state.capabilities,
                            static_cast<unsigned long long>(sample.owner));
                    }
                    return result;
                }
                int32_t selectAndVerify(uint32_t hz){
                    session.event("clock-excursion-transition-Hz",hz);
                    return session.rate(hz,[&]{return stop.requested();});
                }
            } backend{session,stop};
            const auto result=Pro40::clockExcursion48k(backend,session.owner);
            switch(result.error) {
            case Pro40::ClockExcursionError::none:break;
            case Pro40::ClockExcursionError::cancelled:status=kIOReturnAborted;break;
            case Pro40::ClockExcursionError::deadline:status=kIOReturnTimeout;break;
            case Pro40::ClockExcursionError::io:status=result.io;break;
            case Pro40::ClockExcursionError::mismatch:status=kIOReturnNotPermitted;break;
            }
            std::fprintf(log,"clock-excursion-result version=1 phase=%u error=%u attempts=%u restoration-required=%u status=%08x\n",
                unsigned(result.phase),unsigned(result.error),result.rateAttempts,unsigned(result.restorationRequired),unsigned(status));
        } else status=options.verifyExistingClockOnly?
            session.verifyExisting48k([&]{return stop.requested();}):session.rate(48000);
    }
    session.startupTrace.phase(Pro40::StartupNotificationTrace::Phase::preparation);
    if(!status && !stop.requested())meterRoutes(session,"after-clock");
    if(!status && !stop.requested())inputBoundary(Pro40::InputBoundaryTrace::Stage::afterClock,session.owner);
    if(!status && stop.requested())status=kIOReturnAborted;
    session.event(options.clockExcursion?"duplex-clock-excursion-verified":options.verifyExistingClockOnly?"duplex-existing-clock-verified":"duplex-clock-refreshed",unsigned(status));
    IOReturn cleanup=0,evidence=0;
    using ShutdownClock=std::chrono::steady_clock;
    std::optional<ShutdownClock::time_point> captureExit;
    auto stopReason=Pro40::StopReason::error;
    auto clean=[&](IOReturn result){if(result && !cleanup)cleanup=result;};
    auto observeIdle=[&](uint64_t expectedOwner){
        if(options.postReleasePeaks && !status && !evidence && !cleanup && stopReason==StopReason::completed){
            postReleaseStarted=true;postReleaseOrigin=session.startupTrace.now();
            auto elapsed=[&]{return session.startupTrace.now()-postReleaseOrigin;};
            while(postRelease.state()==PostReleasePeakWindow::State::pending){
                postRelease.advance(elapsed(),stop.requested() || (options.ownedIdleObservation && session.invalidPacket),[&](ControlInputPeak &peak){
                    peak.capture(inputRead,[&]{return inputGuard(expectedOwner);});
                },elapsed);
                if(postRelease.state()==PostReleasePeakWindow::State::pending)
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.01,false);
            }
            // Observation status is independent of native resource cleanup. Never
            // reset its read budget or reacquire ownership to complete a window.
        }
        if(options.preCloseQuietHold && cleanupOriginSet && postReleaseStarted &&
           postRelease.state()==PostReleasePeakWindow::State::complete &&
           !status && !evidence && !cleanup && stopReason==StopReason::completed){
            quietStarted=true;quietBegin=session.startupTrace.now();
            PreCloseQuietHold hold(quietBegin-cleanupOrigin);
            quietState=hold.state();
            while(quietState==PreCloseQuietHold::State::pending){
                quietState=hold.advance(session.startupTrace.now()-cleanupOrigin,stop.requested() || (options.ownedIdleObservation && session.invalidPacket));
                if(quietState==PreCloseQuietHold::State::pending)
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.01,false);
            }
            quietEnd=session.startupTrace.now();
        }
    };
    uint64_t restoreBegin=0,restoreEnd=0,restoreHostBegin=0,restoreHostEnd=0;
    IOReturn assignedGuard=kIOReturnNotReady;

    StartupRecovery recovery;
    if(options.startupRecovery)recovery.noteIdle(session.bits);
    const bool needsRecovery=recovery.pending();
    bool recoveryRequested=false;
    for(unsigned attempt=0;!status && attempt<(needsRecovery?2u:1u);++attempt) {
        const bool prelude=needsRecovery && attempt==0;
        options=requestedOptions;
        if(prelude) {
            options.recording=nullptr;options.timing=nullptr;options.warmupOnly=true;
            options.postReleasePeaks=false;options.preCloseQuietHold=false;
            options.ownedIdleObservation=false;options.assignedIdleObservation=false;
        }
        if(requestedOptions.startupRecovery)
            std::fprintf(log,"startup-recovery-attempt version=1 index=%u prelude=%u trigger=%08x quiet-us=400000 owner-retained=1\n",
                attempt,unsigned(prelude),session.bits&3);
        uint64_t readyMicros=0,requestedMicros=0;
        auto device=session.device;
        Pro40Resources::Resources before{},held{},after{};
        status=Pro40Resources::snapshot(device,session.generation,before);
        const bool haveBefore=!status;
        bool dispatcher=false;
        if(!status) {
            status=(*device)->AddIsochCallbackDispatcherToRunLoop(device,CFRunLoopGetCurrent());
            dispatcher=!status;
        }
#ifdef PRO40_DMA_TX_LOCK
        auto statusRead=std::unique_ptr<NativeAsyncStatusRead>(new(std::nothrow) NativeAsyncStatusRead);
        if(!status && !statusRead)status=kIOReturnNoMemory;
        if(!status)status=statusRead->prepare(device,session.generation,session.global);
#endif
        {
            Pro40Resources::NativeReservations resources{device,log};
            std::array<GenerationReservation<Pro40Resources::NativeReservations>,4> reservations;
            auto &dma=*dmaStorage;
            std::array<uint8_t,4> channels{};uint64_t chosen=0;
            UInt32 timer=0;UInt16 local=0;
            if(!status)status=(*device)->GetLocalNodeIDWithGeneration(device,session.generation,&local);
            if(!status)status=(*device)->GetCycleTime(device,&timer);
            if(!status && (((timer>>12)&0x1fff)>=8000 || (local&63)==63))status=kIOReturnError;
            const auto target=((((timer>>25)&3)*8000+((timer>>12)&0x1fff))+initialLead)%32000;
            const uint32_t start=(target/8000)<<25|(target%8000)<<12;
            std::fprintf(log,"duplex-schedule target=%08x local=%04x generation=%u descriptors=%u\n",
                start,local,session.generation,txCount);
#if defined(PRO40_DMA_DUPLEX_REPLAY) || defined(PRO40_DMA_TX_RETURN)
            std::fprintf(log,"duplex-geometry rx=%u tx=%u\n",rxCount,txCount);
#endif
            for(unsigned i=0;!status && i<4;++i) {
                if(stop.requested()){status=kIOReturnAborted;break;}
                unsigned c=0;
                for(;c<63;++c)if((before.channels&(1ULL<<(63-c))) && !(chosen&(1ULL<<(63-c))))break;
                if(c==63){status=kIOReturnNoResources;break;}
                channels[i]=uint8_t(c);chosen|=1ULL<<(63-c);
                status=reservations[i].acquire(resources,session.generation,channels[i]);
#ifdef PRO40_DMA_SOURCE_QUEUE
                dma[i].setHistoryEnabled(retainHistory);
#endif
                if(!status)status=dma[i].prepare(device,log,i,c,start,uint8_t(local&63),session.generation);
                if(i<2)dma[i].setCapture(captures[i]);
#ifdef PRO40_DMA_RX_REUSE
                if(i<2)dma[i].setFinalCapture(captures[captures.size()-2+i]);
#endif
            }
            if(!status)status=Pro40Resources::snapshot(device,session.generation,held);
            if(!status && (before.bandwidth<2048 || held.bandwidth!=before.bandwidth-2048 ||
                held.channels!=(before.channels&~chosen)))status=kIOReturnError;
            session.event("duplex-prepared",unsigned(status));
#ifdef PRO40_DMA_DUPLEX_REPLAY
            constexpr unsigned replayCount=txCount-384;
            std::array<std::vector<NativePackets::PreparedBuffer>,2> prepared;
            for(auto &stream:prepared)stream.resize(replayCount);
#endif
#ifdef PRO40_DMA_TX_REFRESH
            auto &refreshed=*refreshedStorage;
#ifdef PRO40_DMA_SOURCE_QUEUE
            std::fprintf(log,"transmit-scratch streams=2 slots-per-stream=%u bytes=%zu batches=%u\n",
                refreshSlots,sizeof(RefreshStorage),DmaPreparation::TransmitBankPolicy::count);
#endif
#ifndef PRO40_DMA_TX_MEASURED
            for(unsigned stream=0;!status && stream<2;++stream) {
                Pro40Packets::Am824Stream content(Pro40Packets::playback[stream],uint8_t(local&63));
                for(unsigned i=0;i<127;++i) {
                    const unsigned logical=firstRefreshCycle+i;
                    const uint8_t blocks=logical%4?8:0;
                    const auto syt=blocks?*Pro40Packets::replaySyt(0,(target+logical)%8000):0xffff;
                    auto &packet=refreshed[stream][i];
                    const auto length=content.make(packet.bytes,blocks,syt);
                    if(!length){status=kIOReturnError;break;}
                    packet.length=uint32_t(*length);
                }
            }
#endif
#endif
#ifdef PRO40_DMA_TX_MEASURED
            DiagnosticRecords<MeasuredDiagnostic,2*DmaPreparation::TransmitBankPolicy::count> measuredDiag(retainHistory);
            if(!measuredDiag.valid())status=kIOReturnNoMemory;
            std::array<std::optional<Pro40Packets::NativeReplay>,2> measuredReplay;
#ifdef PRO40_DMA_SOURCE_QUEUE
            std::optional<Pro40Audio::NativeInputDelivery> inputDelivery;
            if(!status && options.inputMonitor) {
                status=dma[0].enableSourceObserver();
                if(!status)inputDelivery.emplace(channels[0],uint8_t(session.remote&63),session.generation,
                                                *options.inputMonitor,options.inputHandoff);
            }
#endif
            for(unsigned stream=0;stream<2;++stream)
                measuredReplay[stream].emplace(stream,channels[stream],uint8_t(session.remote&63),uint8_t(local&63),session.generation,options.recording,options.timing?&options.timing->packets:nullptr,!options.warmupOnly && !options.sustained,stream==0?options.playback:nullptr,stream==0?options.livePlayback:nullptr);
            std::fprintf(log,"transmit-measured source-first=0 count=%u output-first=%u\n",refreshCount,firstRefreshCycle);
#endif
#ifdef PRO40_DMA_TX_LOCK
            struct RefillLockSample {uint32_t progress=0,cycle=0,clock=0,extended=0,rate=0;IOReturn result=0;std::array<uint64_t,4> micros{};unsigned measured=0,started=0;std::array<unsigned,4> iterations{};};
            // Isolated startup diagnostic: retain already-issued quadlet reads.
            // No extra transaction, altered cadence or active-loop logging.
#ifdef PRO40_STARTUP_TIMELINE
            DiagnosticRecords<RefillLockSample,Pro40Packets::PlaybackLockCheck::observationCapacity(txCount)> refillLockSamples(true);
#else
            DiagnosticRecords<RefillLockSample,Pro40Packets::PlaybackLockCheck::observationCapacity(txCount)> refillLockSamples(retainHistory);
#endif
            if(!refillLockSamples.valid())status=kIOReturnNoMemory;
            unsigned refillLockCount=0,nextRefillLock=128;
            std::fprintf(log,"refill-lock-mode staged-completion async=1\n");
            std::fprintf(log,"refill-lock-steady-schedule boundary-spacing=%u start-allowance=128 completion-span=%u\n",Pro40Packets::PlaybackLockCheck::cadenceCycles,Pro40Packets::StagedLockPoll::maximumSpanCycles);
            std::fprintf(log,"refill-lock-startup-schedule start-spacing=64 completion-acceptance=1 content-start=%u acquisition-allowance=256 completion-span=%u\n",firstRefreshCycle,Pro40Packets::StagedLockPoll::maximumSpanCycles);
            std::optional<Pro40Packets::StagedLockPoll> pendingPoll;
            RefillLockSample pendingSample{};
            Pro40Packets::PlaybackLockCheck refillLockCheck(txCount,firstRefreshCycle);
            Pro40Packets::QuietStartupControl quietControl;
            std::array<StagedMeterRoutes,2> activeRoutes{
                StagedMeterRoutes(session.generation,session.global,8000,txCount),
                StagedMeterRoutes(session.generation,session.global,32000,txCount)};
            std::array<ProbeCost,2> activeRouteCosts{};
#endif
            std::fprintf(log,"stream-ownership policy=verified-generation-lease refill-owner-reads=0 cleanup-owner-read=1\n");
            ProbeCost generationCost;
#ifdef PRO40_DMA_RX_HANDOFFS
            ProbeCost receiveGuardCost;
#endif
#ifdef PRO40_DMA_TX_RETURN
            constexpr unsigned txHandoffCount=DmaPreparation::TransmitBankPolicy::count;
            DiagnosticRecords<DmaPreparation::TransmitReturn,2*txHandoffCount> txReturns(retainHistory);
            if(!txReturns.valid())status=kIOReturnNoMemory;
            unsigned txHandoff=0;
            std::array<DmaPreparation::TransmitReturn,2> firstTransmitReturns{};
            std::array<DmaPreparation::TransmitReturn,2> slowestTransmitReturns{};
            std::array<unsigned,2> slowestTransmitOrdinals{};
            unsigned firstTransmitReturnMask=0;
            std::array<Pro40::TransmitMarginSummary,2> transmitMargins{};
            std::array<unsigned,2> earlyWaitCount{},earlyWaitReads{},earlyRevalidations{};
            std::array<uint64_t,2> earlyWaitMaximum{};
#ifdef PRO40_DMA_TX_MEASURED
            // First-bank diagnostic only: fixed storage, emitted after all DMA stops.
            std::array<std::array<uint64_t,3>,2> firstPreparationMicros{};
            std::array<unsigned,2> firstPreparationMask{};
            uint64_t firstGuardMicros=0;bool firstGuardMeasured=false;
#endif
#endif
#ifdef PRO40_DMA_RX_BRANCH_EXTEND
#ifdef PRO40_DMA_RX_HANDOFFS
            static constexpr unsigned handoffCount=DmaPreparation::ReceiveBankPolicy::count;
            using ReceiveRecord=NativePackets::ReceiveHandoffRecord<DmaPreparation::ReceiveExtension>;
            DiagnosticRecords<ReceiveRecord,2*handoffCount> extensions(retainHistory);
            unsigned extensionCount=0;
            if(!extensions.valid())status=kIOReturnNoMemory;
            std::array<unsigned,2> handoff{};
#else
            std::array<DmaPreparation::ReceiveExtension,2> extensions{};
            std::array<bool,2> extended{};
#endif
#endif
            ConfigBackend configIO{session};Pro40ChannelConfig config(configIO);
            EnableBackend enableIO{configIO,target,stop};Pro40StreamEnable enabled(enableIO);
            if(options.enableFirstStartup){enableIO.enableMinimum=768;enableIO.enableMaximum=1024;}
            if(!status)status=config.configure(channels);
            session.event("duplex-configured",unsigned(status));
            if(!status && (options.timing || options.inputHandoff)) {
                // Local controller observation before any receive port starts.
                // Apple IOFireWireLibDevice.cpp:1005-1017 uses kCycleTime;
                // no remote transaction, stream write or callback-side IO.
                NativeTimingEvidence::Clock pair;
                pair.generation=session.generation;pair.beforeReceive=true;
                auto readStatus=session.stable();
                if(!readStatus) {
                    pair.before=mach_absolute_time();
                    readStatus=(*device)->GetCycleTimeAndUpTime(device,&pair.cycle,&pair.uptime);
                    pair.after=mach_absolute_time();
                    if(!readStatus)readStatus=session.stable();
                }
                pair.status=uint32_t(readStatus);
                if(options.timing)options.timing->observe(pair);
                if(options.inputHandoff && !options.inputHandoff->clock({pair.before,pair.uptime,pair.after,
                    pair.generation,pair.cycle,pair.status,0,true}))status=kIOReturnError;
            }
            if(!status&&options.timing&&options.timing->consumerFailed())status=kIOReturnError;
            if(!status&&options.inputHandoff && options.inputHandoff->fault()!=Pro40Audio::SustainedInputFault::none)status=kIOReturnError;
            if(!options.enableFirstStartup)for(unsigned i=0;!status && i<4;++i) {
                status=configIO.guard();uint32_t lead=0;
                if(!status)status=enableIO.lead(lead);
                if(!status)status=startupStatus(stop.requested(),lead,768,initialLead);
                if(!status)status=dma[i].start();
            }
            if(!options.enableFirstStartup)session.event("duplex-armed",unsigned(status));
            const auto waitLimit=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(!status) {
                uint32_t lead=0;status=enableIO.lead(lead);
                if(!status)status=startupStatus(stop.requested(),lead,128,initialLead,
                    std::chrono::steady_clock::now()>waitLimit);
                if(status || lead<=enableIO.enableMaximum)break;
                CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.001,true);
            }
            session.startupTrace.phase(Pro40::StartupNotificationTrace::Phase::enable);
            if(!status && options.enableFirstStartup) {
                // Reference-backed order, keeping the existing scheduled TX target.
                // Before/after guards prevent a blocking Start crossing admission.
                // No retries, rescheduling, or extra fixed vendor delay.
                struct DiagnosticStart {
                    EnableBackend &io;
                    Pro40StreamEnable<EnableBackend> &enable;
                    decltype(dma) ports;
                    std::chrono::steady_clock::time_point deadline;
                    std::array<uint32_t,8> leads{};
                    unsigned checks=0,attempted=0,receiveChecks=0;
                    uint32_t receiveLead=0;
                    int32_t enableDevice(){
                        const auto result=enable.enable();
                        io.config.session.event("diagnostic-device-enabled",unsigned(result));
                        return result;
                    }
                    int32_t guard(uint32_t maximum,uint32_t &lead){
                        if(io.stop.requested())return kIOReturnAborted;
                        if(std::chrono::steady_clock::now()>=deadline)return kIOReturnTimeout;
                        auto &s=io.config.session;
                        Pro40ClockState current{};
                        auto result=s.stable();
                        if(!result)result=s.state(current);
                        if(!result && (s.ownerOf(current)!=s.owner || current.enabled!=1 ||
                            current.selected!=0x20c || current.measured!=48000 || current.status!=0x201))
                            result=kIOReturnNotPermitted;
                        if(!result)result=s.stable();
                        if(!result)result=io.lead(lead);
                        if(!result)result=startupStatus(io.stop.requested(),lead,128,maximum,
                            std::chrono::steady_clock::now()>=deadline);
                        return result;
                    }
                    int32_t startGuard(unsigned index){
                        uint32_t lead=0;
                        const auto result=guard(index>=2?1024:512,lead);
                        if(checks<leads.size())leads[checks++]=lead;
                        return result;
                    }
                    int32_t receiveGuard(uint32_t &lead){
                        const auto result=guard(1024,lead);
                        ++receiveChecks;receiveLead=lead;
                        return result;
                    }
                    void pumpReceiveWait(){
                        CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.001,true);
                    }
                    int32_t startHost(unsigned index){++attempted;return ports[index].start();}
                } startIO{enableIO,enabled,dma,waitLimit};
                // Explicit marker makes this evidence distinguishable even on failure.
                session.event("diagnostic-enable-first",2);
                status=Pro40::enableFirstStartup(startIO);
                std::fprintf(log,"enable-first-receive checks=%u lead=%u\n",startIO.receiveChecks,startIO.receiveLead);
                std::fprintf(log,"enable-first-startup attempted=%u checks=%u status=%08x leads=",
                    startIO.attempted,startIO.checks,unsigned(status));
                for(unsigned i=0;i<startIO.checks;++i)std::fprintf(log,"%s%u",i?",":"",startIO.leads[i]);
                std::fprintf(log,"\n");
                session.event("duplex-armed",unsigned(status));
            } else if(!status)status=enabled.enable();
            if(!status)session.startupTrace.phase(Pro40::StartupNotificationTrace::Phase::streaming);
            if(!status && !lifecycle.running())status=kIOReturnError;
            session.event("duplex-enabled",unsigned(status));
#ifdef PRO40_DMA_DUPLEX_REPLAY
            Pro40Packets::RollingReplay replay({channels[0],channels[1]},uint8_t(session.remote&63),
                uint8_t(local&63),session.generation,(target+384)%32000);
            bool tailApplied=false;
            // Apple IOFWUserIsochPort.cpp:950-954 sends each NuDCL callback
            // through the async Mach port. Observe queue occupancy without
            // changing its limit or draining messages outside the dispatcher.
            const auto callbackPort=(*device)->GetIsochAsyncPort(device);
            struct Update {unsigned first=0,port=0,before=0,after=0;IOReturn result=0;
                CallbackQueueState queueBefore,queueAfter;uint64_t publishMicros=0;
                DmaPreparation::UpdateTiming timing;};
            std::array<Update,2*replayCount/64> updates{};unsigned updateCount=0;
            // Linux dice-interface.h:134-168: extended ARX lock/slip bits.
            // Sparse control reads only after tail updates; emit after Stop.
            struct LockSample {uint32_t cycle=0,status=0,extended=0,rate=0,progress=0;IOReturn result=0;};
            std::array<LockSample,txCount/128+8> lockSamples{};unsigned lockCount=0,nextLockProgress=384;
            ProbeCost ownerCost,composeCost,publishCost,lockCost;
            struct Backlog {unsigned offset=0,rx0=0,rx1=0;};
            std::array<Backlog,replayCount/64> backlogs{};unsigned backlogCount=0;
#endif
#ifdef PRO40_DMA_SOURCE_QUEUE
            // Finite profiles retain their deadline. Sustained mode monitors
            // each port's progress with the same 576 ms stall allowance.
            constexpr uint64_t watchdogMicros=uint64_t(txCount)*125+576000;
            if(options.sustained)std::fprintf(log,"sustained-run counter-limit=%u stall-us=%llu stop-after-seconds=%u\n",txCount,
                static_cast<unsigned long long>(StreamProgressWatchdog::stallMicros),options.stopAfterSeconds);
            else std::fprintf(log,"bounded-run cycles=%u stream-us=%llu watchdog-us=%llu\n",txCount,
                static_cast<unsigned long long>(uint64_t(txCount)*125),static_cast<unsigned long long>(watchdogMicros));
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::microseconds(watchdogMicros);
#else
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1600);
#endif
            unsigned loopCount=0,dispatchCount=0,dispatchedCallbacks=0,maxDispatchCallbacks=0;
            uint64_t dispatchMicros=0,maxDispatchMicros=0,slowestDispatchCpuNS=0;
            struct DispatchDelay {
                unsigned iteration=0,callbacks=0;uint64_t wallMicros=0,cpuNS=0;
#ifdef PRO40_DUPLEX_WORKER
                DispatchWaitSample waiting{};
#endif
            };
            std::array<DispatchDelay,8> dispatchDelays{};unsigned dispatchDelayCount=0;
            [[maybe_unused]] bool sustainedStop=false;
#ifdef PRO40_DMA_SOURCE_QUEUE
            struct LoopPoint {uint64_t micros=0;std::array<unsigned,4> progress{};};
            struct LoopRecord {unsigned ordinal=0,mask=0;std::array<LoopPoint,6> points{};};
            // Last eight iterations only, with no allocation or hot-path IO.
            // Phase0=start,1=afterTX,2=afterRX,3=afterLock,4=beforeDispatch,
            // 5=afterDispatch. Missing phases locate an early failure.
            std::array<LoopRecord,8> loopTail{};
            const auto traceStart=std::chrono::steady_clock::now();
            StreamProgressWatchdog progressWatchdog(0);
            auto tracePoint=[&](unsigned phase) {
                auto &record=loopTail[(loopCount-1)%loopTail.size()];
                if(phase==0){record={};record.ordinal=loopCount;}
                record.mask|=1u<<phase;
                auto &point=record.points[phase];
                point.micros=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now()-traceStart).count());
                for(unsigned p=0;p<4;++p)point.progress[p]=dma[p].callbacks();
            };
#endif
            // Dedicated managed native workers stream once per process.
#ifdef PRO40_DUPLEX_WORKER
            if(!status && !nativeScheduling.enter())status=kIOReturnError;
#endif
            while(!status) {
#ifdef PRO40_DUPLEX_WORKER
                if(!nativeScheduling.healthy()){status=kIOReturnError;break;}
#endif
                ++loopCount;
                if(retiredDmaStorage)for(const auto &port:*retiredDmaStorage)
                    if(port.callbackStatus())status=kIOReturnError;
                if(status)break;
                if(options.timing && options.timing->pollObserver)
                    options.timing->pollObserver(options.timing->observerContext);
                if(options.timing&&!options.timing->pollConsumer()){status=kIOReturnError;break;}
                if(options.inputHandoff && options.inputHandoff->fault()!=Pro40Audio::SustainedInputFault::none) {status=kIOReturnError;break;}
#ifdef PRO40_DMA_SOURCE_QUEUE
                tracePoint(0);
#endif
                if(session.invalidPacket){status=kIOReturnError;stopReason=Pro40::StopReason::invalidNotification;break;}
                if(!options.sustained && std::chrono::steady_clock::now()>deadline){status=kIOReturnTimeout;stopReason=Pro40::StopReason::watchdog;break;}
                if(stop.requested()){(void)lifecycle.requestStop();status=kIOReturnAborted;stopReason=Pro40::StopReason::interrupted;break;}
#ifdef PRO40_DMA_SOURCE_QUEUE
                if(options.sustained) {
                    const auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now()-traceStart).count());
                    const std::array<unsigned,4> counts={dma[0].callbacks(),dma[1].callbacks(),dma[2].callbacks(),dma[3].callbacks()};
                    if(!progressWatchdog.observe(elapsed,counts)) {
                        status=kIOReturnTimeout;stopReason=Pro40::StopReason::watchdog;break;
                    }
                    if(options.stopAfterSeconds && elapsed>=uint64_t(options.stopAfterSeconds)*1000000) {
                        sustainedStop=true;break;
                    }
                }
#endif
#ifdef PRO40_DMA_SOURCE_QUEUE
                // Consume copied RX records before a TX bank can acknowledge
                // them. Bounded to one bank; no callback-side content or IO.
                if(inputDelivery && !inputDelivery->drain(dma[0].availableSourcePackets(),session.generation,
                    [&](uint64_t n,NativePackets::PreparedBuffer &copy,uint32_t &s,uint32_t &stamp) {
                        return dma[0].observeCompletedReceive(n,copy,s,stamp)==0;
                    })) {status=kIOReturnError;break;}
#endif
#ifdef PRO40_DMA_TX_RETURN
                if(dma[2].transmitVisitFault() || dma[3].transmitVisitFault()) {status=kIOReturnError;break;}
                if(txHandoff<txHandoffCount &&
#ifdef PRO40_DMA_SOURCE_QUEUE
                   NativePackets::transmitBankReady(txHandoff,dma[2].callbacks(),dma[3].callbacks(),DmaPreparation::transmitBankPackets)
#else
                   dma[2].callbacks()>=(txHandoff+1)*DmaPreparation::transmitBankPackets && dma[3].callbacks()>=(txHandoff+1)*DmaPreparation::transmitBankPackets
#endif
#ifdef PRO40_DMA_TX_MEASURED
                   && dma[0].callbacks()>=(txHandoff+1)*DmaPreparation::transmitBankPackets-(firstRefreshCycle-transmitSlots) && dma[1].callbacks()>=(txHandoff+1)*DmaPreparation::transmitBankPackets-(firstRefreshCycle-transmitSlots)
#endif
                ) {
#ifdef PRO40_DMA_TX_MEASURED
                    const unsigned sourceFirst=txHandoff?firstRefreshCount+(txHandoff-1)*transmitBankPackets:0,sourceEnd=(txHandoff+1)*DmaPreparation::transmitBankPackets-(firstRefreshCycle-transmitSlots);
                    for(unsigned stream=0;!status && stream<2;++stream) {
                        auto &diag=measuredDiag[txHandoff*2+stream];
                        auto &replay=*measuredReplay[stream];
                        auto phaseStart=std::chrono::steady_clock::time_point{};
                        bool phaseAttempted=false;
                        auto beginFirstPhase=[&] {
                            phaseAttempted=txHandoff==0 && !status;
                            if(phaseAttempted)phaseStart=std::chrono::steady_clock::now();
                        };
                        auto endFirstPhase=[&](unsigned phase) {
                            if(phaseAttempted) {
                                firstPreparationMicros[stream][phase]=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now()-phaseStart).count());
                                firstPreparationMask[stream]|=1u<<phase;
                            }
                        };
                        beginFirstPhase();
                        for(unsigned n=sourceFirst;!status && n<sourceEnd;++n) {
                            diag.stage=1;diag.index=n;
                            status=dma[stream].copyCompletedReceive(n,diag.copy,diag.status,diag.stamp);
                            if(!status) {
                                diag.stage=2;
                                if(replay.ingest(diag.copy.bytes,diag.status,diag.stamp,session.generation)!=Pro40Packets::NativeReplayError::none)
                                    status=kIOReturnError;
                                diag.transport=unsigned(replay.transportError());diag.sequence=unsigned(replay.sequenceError());
                            }
                        }
                        endFirstPhase(0);
                        beginFirstPhase();
                        // 32000-cycle scheduling ambiguity does not affect SYT:
                        // 32000 is divisible by its sixteen-cycle period.
                        if(!status)diag.stage=3;
                        if(!status && txHandoff==0 && replay.start((target+firstRefreshCycle)%64000,session.generation)!=Pro40Packets::ReplayError::none)
                            status=kIOReturnError;
                        endFirstPhase(1);
                        beginFirstPhase();
                        for(unsigned n=sourceFirst;!status && n<sourceEnd;++n) {
                            diag.stage=4;diag.index=n;
                            // returnTransmitBank copies into its retained DMA mapping;
                            // no pointer into this scratch batch survives publication.
                            auto &packet=refreshed[stream][n%refreshSlots];
                            const auto result=replay.produce(packet.bytes,(target+firstRefreshCycle+n)%64000,session.generation);
                            if(!result)status=kIOReturnError;
                            else packet.length=uint32_t(result.bytes);
                        }
                        endFirstPhase(2);
                        if(!status)diag.stage=5;
                    }
#endif
                    if(!status) {
#ifdef PRO40_DMA_TX_MEASURED
                        auto guardStart=std::chrono::steady_clock::time_point{};
                        if(txHandoff==0)guardStart=std::chrono::steady_clock::now();
#endif
                        status=enableIO.streamGuard();
#ifdef PRO40_DMA_TX_MEASURED
                        if(txHandoff==0) {
                            firstGuardMicros=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now()-guardStart).count());
                            firstGuardMeasured=true;
                        }
#endif
                    }
                    for(unsigned stream=0;!status && stream<2;++stream) {
                        status=dma[stream+2].returnTransmitBank(device,txReturns[txHandoff*2+stream],
                            [&]() -> IOReturn {return stop.requested()?IOReturn(kIOReturnAborted):enableIO.streamGuard();}
#ifdef PRO40_DMA_TX_REFRESH
                            ,std::span(refreshed[stream]).subspan((txHandoff?firstRefreshCount+(txHandoff-1)*transmitBankPackets:0)%refreshSlots,txHandoff?transmitBankPackets:firstRefreshCount)
#endif
                        );
                        const auto &sample=txReturns.read(txHandoff*2+stream);
                        if(sample.micros>=slowestTransmitReturns[stream].micros) {
                            slowestTransmitReturns[stream]=sample;slowestTransmitOrdinals[stream]=txHandoff;
                        }
                        earlyRevalidations[stream]+=unsigned(sample.earlyRevalidated);
                        if(sample.earlyReads || sample.earlyMicros) {
                            ++earlyWaitCount[stream];earlyWaitReads[stream]+=sample.earlyReads;
                            earlyWaitMaximum[stream]=std::max(earlyWaitMaximum[stream],sample.earlyMicros);
                        }
                        transmitMargins[stream].observe(txHandoff,sample.before,sample.cutLead,sample.after,sample.micros,sample.result);
                        if(txHandoff==0) {
                            firstTransmitReturns[stream]=txReturns.read(stream);
                            firstTransmitReturnMask|=1u<<stream;
                        }
                    }
#ifdef PRO40_DMA_SOURCE_QUEUE
                    // Both outputs now own their prepared bytes. Release input
                    // groups only after both native publications succeeded.
                    for(unsigned stream=0;!status && stream<2;++stream)
                        status=dma[stream].acknowledgeSource(sourceFirst);
#endif
                    if(!status)++txHandoff;
                }
#ifdef PRO40_DMA_SOURCE_QUEUE
                tracePoint(1);
#endif
                if(status)break;
#endif
#ifdef PRO40_DMA_RX_BRANCH_EXTEND
#ifdef PRO40_DMA_RX_REUSE
                if(dma[0].reuseFault() || dma[1].reuseFault()) {status=kIOReturnError;break;}
#endif
#ifdef PRO40_DMA_RX_HANDOFFS
                // Each native port owns its bank graph and first timestamp.
                // Apple IOFWUserIsochPort.cpp:1168-1337 applies notifications to
                // that local port. Waiting for its peer can consume its cushion.
                for(unsigned stream=0;!status && stream<2;++stream) {
                    if(!DmaPreparation::ReceiveBankPolicy::handoffPending(
                        handoff[stream],dma[stream].callbacks()))continue;
                    status=receiveGuardCost.measure([&]{return enableIO.streamGuard();});
                    if(!status) {
                        auto &record=extensions[extensionCount++];
                        record.stream=stream;record.ordinal=handoff[stream];
                        status=dma[stream].extendReceiveBranch(device,record.sample);
                        if(!status)++handoff[stream];
                    }
                }
#else
                if(!extended[0] && !extended[1] && dma[0].callbacks()>=DmaPreparation::receiveCopyCount &&
                    dma[1].callbacks()>=DmaPreparation::receiveCopyCount) {
                    // One session owns both ports. Avoid an extra synchronous
                    // device transaction between the two time-critical updates.
                    status=enableIO.streamGuard();
                    for(unsigned stream=0;!status && stream<2;++stream) {
                        if(!status)status=dma[stream].extendReceiveBranch(device,extensions[stream]);
                        if(!status)extended[stream]=true;
                    }
                }
#endif
#ifdef PRO40_DMA_SOURCE_QUEUE
                tracePoint(2);
#endif
                if(status)break;
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
                if(dma[0].captureFault() || dma[1].captureFault()) {status=kIOReturnError;break;}
                const auto offset=unsigned(replay.sourceOffset());
                if(offset<replayCount && dma[0].callbacks()>=offset+64 && dma[1].callbacks()>=offset+64) {
                    backlogs[backlogCount++]={offset,dma[0].callbacks(),dma[1].callbacks()};
                    status=ownerCost.measure([&]{return enableIO.streamGuard();});
                    if(!status && composeCost.measure([&]{return replay.prepare([&](unsigned stream,uint64_t n){
                        return dma[stream].capturedPacket(unsigned(n));
                    },{dma[0].callbacks(),dma[1].callbacks()},session.generation);})!=Pro40Packets::RefillResult::ready)
                        status=kIOReturnError;
                    for(unsigned stream=0;!status && stream<2;++stream) {
                        auto &update=updates[updateCount++];update.first=384+offset;update.port=stream+2;
                        uint32_t lead=0;status=session.stable();
                        if(!status)status=enableIO.lead(lead);
                        update.before=(lead+update.first)%32000;
                        if(!status && (stop.requested() || std::chrono::steady_clock::now()>deadline ||
                            update.before<128 || update.before>4000))status=kIOReturnTimeout;
                        const auto batch=replay.batch(stream);
                        if(!status) {
                            update.queueBefore.read(callbackPort);
                            const auto previous=publishCost.total;
                            status=publishCost.measure([&]{return dma[stream+2].replaceFutureBatch(update.first,batch,update.timing);});
                            update.publishMicros=publishCost.total-previous;
                            update.queueAfter.read(callbackPort);
                        }
                        if(!status)status=enableIO.lead(lead);
                        update.after=(lead+update.first)%32000;
                        if(!status && (update.after<64 || update.after>4000))status=kIOReturnTimeout;
                        update.result=status;
                        if(!status)std::copy(batch.begin(),batch.end(),prepared[stream].begin()+offset);
                    }
                    if(!replay.acknowledge(offset,!status) && !status)status=kIOReturnError;
                    for(unsigned stream=0;!status && stream<2;++stream)
                        status=dma[stream].consumeCapturedBatch(offset);
                    tailApplied=!status && replay.sourceOffset()==replayCount;
                    if(status)break;
                }
                const auto progress=std::min(dma[2].callbacks(),dma[3].callbacks());
                if(replay.sourceOffset()>0 && progress>=nextLockProgress && lockCount<lockSamples.size()) {
                    auto &sample=lockSamples[lockCount++];sample.progress=progress;
                    std::array<uint8_t,12> words{};
                    sample.result=lockCost.measure([&]{return pro40_read(session.generation,session.global+0x54,12,words.data());});
                    if(!sample.result)sample.result=(*device)->GetCycleTime(device,&sample.cycle);
                    if(!sample.result) {
                        sample.status=ownedBe32(words.data());sample.extended=ownedBe32(words.data()+4);
                        sample.rate=ownedBe32(words.data()+8);
                    } else status=sample.result;
                    nextLockProgress=progress+128;
                    if(status)break;
                }
#endif
#ifdef PRO40_DMA_TX_LOCK
                const auto lockProgress=std::min(dma[2].callbacks(),dma[3].callbacks());
                [[maybe_unused]] bool lockReadThisIteration=false;
                if((!options.quietStartupControl || quietControl.pollingAllowed()) &&
                   (pendingPoll || lockProgress>=nextRefillLock) && lockProgress<txCount) {
                    bool deferPoll=false;
#ifdef PRO40_DMA_SOURCE_QUEUE
                    deferPoll=(txHandoff<txHandoffCount && NativePackets::transmitBankPending(
                        txHandoff,dma[2].callbacks(),dma[3].callbacks(),DmaPreparation::transmitBankPackets)) ||
                        DmaPreparation::ReceiveBankPolicy::pollingMustWait(
                            handoff,dma[0].callbacks(),dma[1].callbacks());
#endif
                    if(!pendingPoll && !deferPoll) {
                        if(refillLockCount==refillLockSamples.size()){status=kIOReturnNoSpace;break;}
                        pendingPoll.emplace(session.generation,txCount,refillLockCheck.lastObservation());
                        pendingSample={};
                    }
                    if(pendingPoll) {
                        using Poll=Pro40Packets::StagedLockPoll;
                        const auto result=pendingPoll->advance(lockProgress,session.generation,
                            stop.requested(),deferPoll,[&](unsigned index,uint32_t &value)->Poll::Result {
                                lockReadThisIteration=true;
                                const auto begin=std::chrono::steady_clock::now();
                                IOReturn readStatus=0;
                                if(index<3) {
                                    const auto read=statusRead->step(index,value);
                                    if(!read.ready)return read; // Keep servicing DMA while the bus read completes.
                                    pendingSample.micros[index]=statusRead->completionMicros();
                                    pendingSample.iterations[index]=loopCount;
                                    return read;
                                } else {
                                    // Bracket final timer/publication with captured-generation
                                    // verification; GetCycleTime itself has no generation input.
                                    readStatus=session.stable();
                                    if(!readStatus && (options.timing || options.inputHandoff)) {
                                        // Same staged timer slot, no additional device reads.
                                        // Paired scalar API validated by NativeClockProbe.cpp.
                                        NativeTimingEvidence::Clock pair;
                                        pair.progress=lockProgress;pair.generation=session.generation;
                                        // Same control owner ingests packets and samples clocks;
                                        // this prefix was consumed before the controller read.
                                        pair.packetCount=options.inputHandoff?options.inputHandoff->packetCount():uint32_t(options.timing->packets.records().size());
                                        pair.before=mach_absolute_time();
                                        readStatus=(*device)->GetCycleTimeAndUpTime(device,&value,&pair.uptime);
                                        pair.after=mach_absolute_time();pair.cycle=value;
                                        if(!readStatus)readStatus=session.stable();
                                        pair.status=uint32_t(readStatus);
                                        if(options.timing)options.timing->observe(pair);
                                        if(options.inputHandoff && !options.inputHandoff->clock({pair.before,pair.uptime,pair.after,
                                            pair.generation,pair.cycle,pair.status,pair.packetCount,false}))readStatus=kIOReturnError;
                                    } else {
                                        if(!readStatus)readStatus=(*device)->GetCycleTime(device,&value);
                                        if(!readStatus)readStatus=session.stable();
                                    }
                                }
                                pendingSample.micros[index]=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now()-begin).count());
                                pendingSample.iterations[index]=loopCount;
                                return readStatus;
                            });
                        const auto &observation=pendingPoll->sample();
                        pendingSample.started=observation.started;
                        pendingSample.progress=observation.completed;
                        pendingSample.measured=observation.attempted;
                        pendingSample.clock=observation.values[0];pendingSample.extended=observation.values[1];
                        pendingSample.rate=observation.values[2];pendingSample.cycle=observation.values[3];
                        pendingSample.result=observation.ioStatus;
                        if(result==Poll::State::failed)
                            status=observation.ioStatus?observation.ioStatus:IOReturn(kIOReturnTimeout);
                        else if(result==Poll::State::cancelled || stop.requested())status=kIOReturnAborted;
                        else if(result==Poll::State::complete) {
                            // A first read does not establish an accepted observation.
                            // A query begun by the acquisition deadline has its bounded
                            // staged span to finish; gap/slip/rate checks still run here.
                            if(!refillLockCheck.observe(observation.completed,pendingSample.clock,
                                pendingSample.extended,pendingSample.rate,observation.started))status=kIOReturnError;
                            pendingSample.result=status;
                            refillLockSamples[refillLockCount++]=pendingSample;
                            if(!status && options.quietStartupControl)
                                quietControl.observeAccepted(refillLockCheck,observation.completed);
                            nextRefillLock=refillLockCheck.nextStagedObservation(observation.started);
                            pendingPoll.reset();
                        }
                        if(status)break;
                    }
                }
                if(options.timing && !options.quietStartupControl) {
                    // Optional diagnostic: at most one read in a loop iteration,
                    // never alongside a lock read or while bank work is pending.
                    // 32 cycles is admission slack, not an IO-duration guarantee.
                    bool deferred=true;
#ifdef PRO40_DMA_SOURCE_QUEUE
                    deferred=lockReadThisIteration || pendingPoll.has_value() ||
                        nextRefillLock<=lockProgress || nextRefillLock-lockProgress<32 ||
                        (txHandoff<txHandoffCount && NativePackets::transmitBankPending(
                            txHandoff,dma[2].callbacks(),dma[3].callbacks(),DmaPreparation::transmitBankPackets)) ||
                        DmaPreparation::ReceiveBankPolicy::pollingMustWait(
                            handoff,dma[0].callbacks(),dma[1].callbacks());
#endif
                    for(size_t n=0;n<activeRoutes.size();++n) {
                        auto &probe=activeRoutes[n];
                        const auto attempted=probe.attempted;
                        probe.advance(lockProgress,session.generation,stop.requested(),deferred,
                            [&](uint64_t address,unsigned bytes,uint8_t *data)->int32_t {
                                return activeRouteCosts[n].measure([&]()->int32_t {
                                    auto result=session.stable();
                                    if(!result)result=pro40_read(session.generation,address,bytes,data);
                                    if(!result)result=session.stable();
                                    return result;
                                });
                            });
                        if(probe.attempted!=attempted)deferred=true;
                    }
                }
#endif
#ifdef PRO40_DMA_SOURCE_QUEUE
                tracePoint(3);
#endif
#if defined(PRO40_STARTUP_RECOVERY) && defined(PRO40_DMA_TX_LOCK)
                if(prelude) {
                    const unsigned progress=std::min(dma[2].callbacks(),dma[3].callbacks());
                    const auto now=session.startupTrace.now();
                    if(!readyMicros && progress>=512 && refillLockCheck.validThrough(progress)) {
                        readyMicros=now;recovery.started(now);
                    }
                    const auto decision=recovery.poll(now,session.lastConfigurationMicros);
                    if(decision==StartupRecovery::Decision::timeout){status=kIOReturnTimeout;break;}
                    if(decision==StartupRecovery::Decision::restart) {
                        // A transport fault must never become a successful restart.
                        if(!refillLockCheck.validThrough(progress) || dma[0].reuseFault() || dma[1].reuseFault()) {
                            status=kIOReturnError;break;
                        }
                        recoveryRequested=true;requestedMicros=now;break;
                    }
                }
#endif
                bool complete=true;
                for(auto &port:dma)complete=complete && port.callbacks()==port.expectedCallbacks();
                if(complete)break;
                status=generationCost.measure([&]{return session.stable();});
                if(!status) {
#ifdef PRO40_DMA_SOURCE_QUEUE
                    tracePoint(4);
#endif
                    unsigned before=0;for(auto &port:dma)before+=port.callbacks();
#ifdef PRO40_DUPLEX_WORKER
                    dispatchObserver.begin();
#else
                    const auto began=std::chrono::steady_clock::now();
                    const auto cpuBefore=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
#endif
                    // CFRunLoop.h: return after one handled source. The duration
                    // parameter does NOT bound time inside callbacks or a thread
                    // deschedule. Re-enter DMA maintenance before another source;
                    // the existing bank/timer guards remain authoritative.
#ifdef PRO40_DMA_SOURCE_QUEUE
                    for(auto &port:dma)port.setDispatchActive(true);
#endif
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.001,true);
#ifdef PRO40_DUPLEX_WORKER
                    const auto waitSample=dispatchObserver.finish();
                    const auto cpuNS=waitSample.cpu;
#else
                    const auto cpuAfter=clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
                    const auto cpuNS=cpuAfter>=cpuBefore?cpuAfter-cpuBefore:0;
#endif
#ifdef PRO40_DMA_SOURCE_QUEUE
                    for(auto &port:dma)port.setDispatchActive(false);
#endif
#ifdef PRO40_DUPLEX_WORKER
                    const auto elapsed=waitSample.wall/1000;
#else
                    const auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now()-began).count());
#endif
                    unsigned after=0;for(auto &port:dma)after+=port.callbacks();
                    ++dispatchCount;dispatchedCallbacks+=after-before;
                    maxDispatchCallbacks=std::max(maxDispatchCallbacks,after-before);
                    dispatchMicros+=elapsed;
                    if(elapsed>maxDispatchMicros){maxDispatchMicros=elapsed;slowestDispatchCpuNS=cpuNS;}
                    if(elapsed>2000) {
                        auto &delay=dispatchDelays[dispatchDelayCount++%dispatchDelays.size()];
                        delay={loopCount,after-before,elapsed,cpuNS};
#ifdef PRO40_DUPLEX_WORKER
                        delay.waiting=waitSample;
#endif
                    }
#ifdef PRO40_DMA_SOURCE_QUEUE
                    tracePoint(5);
#endif
                }
            }
#ifdef PRO40_DMA_DUPLEX_REPLAY
            if(!status && !tailApplied)status=kIOReturnNotReady;
#endif
            if(prelude && !recoveryRequested && !status)status=kIOReturnNotReady;
            // Coarse supervisor boundary, not a packet/evidence timestamp.
            if(lifecycle.phase()==Pro40::SessionPhase::running)captureExit=ShutdownClock::now();
#ifdef PRO40_DMA_TX_LOCK
            if(pendingPoll)pendingPoll->advance(0,session.generation,true,true,
                [](unsigned,uint32_t &)->int32_t{return kIOReturnAborted;});
            const unsigned completedProgress=std::min({dma[0].callbacks(),dma[1].callbacks(),dma[2].callbacks(),dma[3].callbacks()});
            const bool lockEvidenceComplete=options.sustained?
                (refillLockCheck.validThrough(std::min(dma[2].callbacks(),dma[3].callbacks()))):options.quietStartupControl?
                quietControl.complete(completedProgress,txCount):refillLockCheck.complete();
            if(!status && !recoveryRequested && !lockEvidenceComplete)status=kIOReturnNotReady;
#endif
            if(!status&&options.timing&&options.timing->consumerFailed())status=kIOReturnError;
            if(!status&&options.inputHandoff && options.inputHandoff->fault()!=Pro40Audio::SustainedInputFault::none)status=kIOReturnError;
            if(!status)stopReason=recoveryRequested?Pro40::StopReason::error:Pro40::StopReason::completed;
            if(status==kIOReturnAborted) {
                (void)lifecycle.requestStop();stopReason=Pro40::StopReason::interrupted;
            }
            if(!lifecycle.beginStop(stopReason,status))status=kIOReturnError;
            // Linux dice-stream.c:465-468 stops the domain before finish_session.
            // Stop every host program before clearing ENABLE, retaining mappings
            // and both dispatchers through native finalization.
            session.startupTrace.phase(Pro40::StartupNotificationTrace::Phase::cleanup);
#if defined(PRO40_DMA_SUSTAINED) && defined(PRO40_DMA_SOURCE_QUEUE) && defined(PRO40_DMA_BIDIRECTIONAL_REUSE) && defined(PRO40_DMA_TX_RETURN)
            // On an ordinary sustained STOP, stop linking banks first, then
            // observe every callback through the already published null tails.
            // Apple IOFWUserIsochPort.cpp:894-922 sends the stop token after
            // super::stop; our90s trace nevertheless had a later TX callback.
            // Explicit null branches:1235-1287; DmaPreparation's
            // cut-before-link ordering is unchanged. No new wire mutations.
            if(options.sustained && status==kIOReturnAborted && stop.requested()) {
                std::array<uint64_t,4> targets{};bool eligible=true;
                for(unsigned p=0;p<dma.size();++p) {
                    const auto target=dma[p].terminalProgress();
                    if(!target){eligible=false;break;}targets[p]=*target;
                }
                if(eligible) {
                    const auto began=ShutdownClock::now();
                    const auto result=NativePackets::drainPublishedStops(targets,began+std::chrono::milliseconds(150),
                        []{return ShutdownClock::now();},[&](auto &progress) {
                            if(session.stable())return false;
                            for(unsigned p=0;p<dma.size();++p) {
                                if(dma[p].terminalProgress()!=targets[p])return false;
                                progress[p]=dma[p].callbacks();
                            }
                            return true;
                        },[]{CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.001,true);});
                    if(result!=NativePackets::StopDrainResult::complete)
                        clean(result==NativePackets::StopDrainResult::timeout?kIOReturnTimeout:kIOReturnError);
                    const auto us=std::chrono::duration_cast<std::chrono::microseconds>(ShutdownClock::now()-began).count();
                    std::fprintf(log,"published-stop-drain result=%u elapsed-us=%llu targets=%llu,%llu,%llu,%llu progress=%u,%u,%u,%u\n",
                        unsigned(result),(unsigned long long)us,(unsigned long long)targets[0],(unsigned long long)targets[1],
                        (unsigned long long)targets[2],(unsigned long long)targets[3],
                        dma[0].callbacks(),dma[1].callbacks(),dma[2].callbacks(),dma[3].callbacks());
                } else std::fprintf(log,"published-stop-drain skipped=1\n");
            }
#endif
            for(auto &port:dma)clean(port.stop());
#ifdef PRO40_DUPLEX_WORKER
            if(!nativeScheduling.close())clean(kIOReturnError);
            const auto &scheduling=nativeScheduling.evidence;
            if(scheduling.set==0 && (scheduling.finalRead || scheduling.starts!=scheduling.finishes))evidence=kIOReturnError;
            std::fprintf(log,"native-audio-scheduling workgroup=0 period-ns=%llu computation-ns=%llu constraint-ns=%llu before=%d set=%d after=%d final=%d restore=%d restored=%d default=%u qos=%u starts=%llu finishes=%llu overruns=%llu max-wall-ns=%llu max-cpu-ns=%llu\n",
                NativeAudioScheduling::periodNS,NativeAudioScheduling::computationNS,NativeAudioScheduling::constraintNS,
                scheduling.before,scheduling.set,scheduling.after,
                scheduling.finalRead,scheduling.restore,scheduling.restoredRead,unsigned(scheduling.restoredDefault),scheduling.restoredQoS,
                scheduling.starts,scheduling.finishes,scheduling.overruns,scheduling.maxWallNS,scheduling.maxCpuNS);
#endif
#ifdef PRO40_DMA_SOURCE_QUEUE
            if(inputDelivery)std::fprintf(log,"early-input packets=%llu max-batch=%u fault=%u transport=%u\n",
                (unsigned long long)inputDelivery->next(),inputDelivery->maximumBatch(),
                unsigned(inputDelivery->fault()),unsigned(inputDelivery->transportError()));
            if(inputDelivery && inputDelivery->startup().prerollOrdinal()!=UINT64_MAX) {
                const auto &s=inputDelivery->startup();
                std::fprintf(log,"input-startup-preroll ordinal=%llu frames=8 bad-pcm-labels=%u bad-midi-labels=%u shifted-labels=%u published=0\n",
                    (unsigned long long)s.prerollOrdinal(),s.badPcmLabels(),s.badMidiLabels(),unsigned(s.shiftedLabels()));
            }
            if(inputDelivery && inputDelivery->rejection().present) {
                const auto &r=inputDelivery->rejection();
                std::fprintf(log,"input-packet-rejection ordinal=%llu size=%u cip=%08x,%08x previous-cip=%08x,%08x arrival=%u previous-arrival=%u parse=%u handoff=%u bad-pcm-labels=%u nonzero-invalid-pcm=%u\n",
                    (unsigned long long)inputDelivery->next(),r.size,r.q0,r.q1,r.previousQ0,r.previousQ1,
                    r.arrival,r.previousArrival,r.parseError,r.handoffFault,r.badPcmLabels,r.nonzeroInvalidPcm);
            }
#endif
#ifdef PRO40_DMA_TX_LOCK
            // A successful Cancel is not callback quiescence. All native buffers
            // and both dispatchers still live here; drain before any release.
            if(statusRead) {
                const auto drainStart=ShutdownClock::now();
                const auto shutdown=statusRead->stopAndDrain(drainStart+std::chrono::milliseconds(100),
                    drainStart+std::chrono::seconds(1),[]{return ShutdownClock::now();},
                    []{CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.001,true);});
                clean(shutdown.cancelStatus);
                std::fprintf(log,"async-status-shutdown pending-at-stop=%u natural=%u cancel-attempted=%u cancel-status=%08x drained=%u completion=%08x\n",
                    unsigned(shutdown.wasPending),unsigned(shutdown.natural),unsigned(shutdown.cancelAttempted),
                    unsigned(shutdown.cancelStatus),unsigned(shutdown.drained),unsigned(shutdown.completionStatus));
                std::fprintf(log,"async-status-read submitted=%u consumed=%u pending=%u max-submit-us=%llu max-completion-us=%llu\n",
                    statusRead->submitted,statusRead->completionsConsumed,unsigned(statusRead->pending()),
                    (unsigned long long)statusRead->maximumSubmitMicros,(unsigned long long)statusRead->maximumCompletionMicros);
                if(!shutdown.drained) {
                    clean(kIOReturnTimeout);
                    // Failed cleanup must block reuse. Keep command/buffer/refcon
                    // alive until process exit: late completion may still arrive
                    // while session.finish drains the shared notification source.
                    // Never pretend an unobserved cancellation freed native use.
                    (void)statusRead.release();
                }
            }
#endif
            if(options.preCloseQuietHold){cleanupOrigin=session.startupTrace.now();cleanupOriginSet=true;}
            // The restoration reserve is single-use per open native session.
            // A planned internal restart uses the remaining normal read budget.
            if(!recoveryRequested || status || cleanup)pro40_begin_cleanup();
            if(requestedOptions.startupRecovery)
                std::fprintf(log,"startup-recovery-stop index=%u requested=%u ready-us=%llu request-us=%llu last-config-us=%llu\n",
                    attempt,unsigned(recoveryRequested),static_cast<unsigned long long>(readyMicros),
                    static_cast<unsigned long long>(requestedMicros),static_cast<unsigned long long>(session.lastConfigurationMicros));
            // Diagnostic only after host DMA stops; never postpone a failed stop cleanup.
            if(!cleanup)meterRoutes(session,"host-stopped-before-disable");
            // Deferred until host stop: reuse the final enable guard's observation.
            // No added register read or IO in the active transfer loop.
            std::fprintf(log,"startup-enable-observation valid=%u lead-cycles=%u\n",
                unsigned(enableIO.enableLeadObserved),enableIO.enableLead);
            clean(enabled.clear());session.event("duplex-disabled",unsigned(cleanup));
            if(!cleanup && !prelude)inputBoundary(Pro40::InputBoundaryTrace::Stage::afterDisable,session.owner);
#ifdef PRO40_DMA_TX_LOCK
            if(options.timing)for(size_t n=0;n<activeRoutes.size();++n) {
                const auto &p=activeRoutes[n];const auto &s=p.snapshot;
                std::fprintf(log,"active-meter-routes index=%zu state=%u error=%u valid=%u io=%08x start=%u complete=%u attempted=%x count=%u enable-before=%u enable-after=%u reads=%u max-us=%llu words=",
                    n,unsigned(p.state()),unsigned(p.error()),unsigned(s.valid),unsigned(s.ioStatus),
                    p.started,p.completed,p.attempted,s.count,s.enableBefore,s.enableAfter,
                    activeRouteCosts[n].calls,static_cast<unsigned long long>(activeRouteCosts[n].maximum));
                for(size_t i=0;i<s.words.size();++i)std::fprintf(log,"%s%08x",i?",":"",s.words[i]);
                std::fprintf(log,"\n");
            }
#endif
#ifdef PRO40_DMA_SOURCE_QUEUE
            if(status) {
                const unsigned first=loopCount>loopTail.size()?loopCount-unsigned(loopTail.size())+1:1;
                for(unsigned n=first;n<=loopCount;++n) {
                    const auto &record=loopTail[(n-1)%loopTail.size()];
                    for(unsigned phase=0;phase<record.points.size();++phase)if(record.mask&(1u<<phase)) {
                        const auto &point=record.points[phase];
                        std::fprintf(log,"loop-tail iteration=%u phase=%u us=%llu rx0=%u rx1=%u tx0=%u tx1=%u\n",
                            record.ordinal,phase,static_cast<unsigned long long>(point.micros),
                            point.progress[0],point.progress[1],point.progress[2],point.progress[3]);
                    }
                }
            }
#endif
#ifdef PRO40_DMA_TX_MEASURED
            if(options.playback)std::fprintf(log,"playback-pcm stream=0 channels=%u source-frames=%zu prepared-frames=%zu\n",
                options.playback->channels,options.playback->frames(),measuredReplay[0]->playbackFramesPrepared());
            if(options.livePlayback)std::fprintf(log,"playback-live stream=0 channels=%u prepared-frames=%zu\n",
                options.livePlayback->channels,measuredReplay[0]->playbackFramesPrepared());
            if(options.sustained) {
                const auto &monitor=*options.inputMonitor;
                std::fprintf(log,"sustained-input packets=%llu frames=%llu fault=%u stopped-on-duration=%u\n",
                    static_cast<unsigned long long>(monitor.packets),static_cast<unsigned long long>(monitor.frames),
                    unsigned(monitor.fault),unsigned(sustainedStop));
                for(unsigned c=0;c<monitor.channels.size();++c) {
                    const auto &v=monitor.channels[c];
                    std::fprintf(log,"sustained-input-channel channel=%u nonzero=%llu first=%llu last=%llu min=%d max=%d\n",
                        c+1,static_cast<unsigned long long>(v.nonzero),static_cast<unsigned long long>(v.firstNonzero),
                        static_cast<unsigned long long>(v.lastNonzero),v.minimum,v.maximum);
                }
                if(monitor.fault || monitor.packets==0)evidence=kIOReturnError;
            }
            if(!options.warmupOnly && !options.sustained) {
            const auto &levels=measuredReplay[0]->inputLevels();
            std::fprintf(log,"input1-observation packets=%llu overflow=%llu\n",
                static_cast<unsigned long long>(levels.packets),static_cast<unsigned long long>(levels.overflow));
            for(unsigned bucket=0;bucket<levels.buckets.size();++bucket) {
                const auto &b=levels.buckets[bucket];
                if(!b.samples && !b.badLabels)continue;
                std::fprintf(log,"input1-level second=%u samples=%llu nonzero=%llu bad-labels=%llu min=%d max=%d\n",
                    bucket,static_cast<unsigned long long>(b.samples),static_cast<unsigned long long>(b.nonzero),
                    static_cast<unsigned long long>(b.badLabels),b.low,b.high);
            }
            // Deferred after host stop, outside packet processing. Fixed-size
            // source copies preserve wire bytes even when PCM export rejects them.
            std::fprintf(log,"input-raw-evidence schema=1 stream=0 slots=11 pcm=10 midi=1 capacity=33\n");
            for(unsigned bucket=0;bucket<levels.rawPackets.size();++bucket){
                const auto &raw=levels.rawPackets[bucket];if(!raw.size)continue;
                std::fprintf(log,"input-raw-packet bucket=%u ordinal=%llu size=%u hex=",
                    bucket,static_cast<unsigned long long>(raw.ordinal),unsigned(raw.size));
                for(unsigned i=0;i<raw.size;++i)std::fprintf(log,"%02x",unsigned(raw.bytes[i]));
                std::fputc('\n',log);
            }
            for(unsigned label=0;label<levels.input1Labels.size();++label)
                if(levels.input1Labels[label])std::fprintf(log,"input-raw-label byte=%02x samples=%llu\n",
                    label,static_cast<unsigned long long>(levels.input1Labels[label]));
#ifdef PRO40_STARTUP_TIMELINE
            const auto &activity=measuredReplay[0]->startupActivity();
            std::fprintf(log,"input-startup-activity schema=1 packets=%llu frames=%llu fault=%u channels=10\n",
                static_cast<unsigned long long>(activity.packets),static_cast<unsigned long long>(activity.frames),unsigned(activity.fault));
            for(size_t n=0;n<activity.channels.size();++n) {
                const auto &c=activity.channels[n];
                const bool found=c.firstNonzeroFrame!=Pro40Packets::InputStartupActivity::absent;
                std::fprintf(log,"input-startup-channel slot=%zu first-valid=%llu found=%u first-frame=%llu packet=%llu block=%u word=%08x invalid-labels=%llu\n",
                    n,static_cast<unsigned long long>(c.firstValidFrame),unsigned(found),
                    static_cast<unsigned long long>(c.firstNonzeroFrame),static_cast<unsigned long long>(c.firstNonzeroPacket),
                    c.block,c.firstWord,static_cast<unsigned long long>(c.invalidLabels));
                if(found) {
                    std::fprintf(log,"input-startup-packet slot=%zu hex=",n);
                    for(auto byte:c.packet)std::fprintf(log,"%02x",unsigned(byte));
                    std::fputc('\n',log);
                }
            }
#endif
            }
#endif
#ifdef PRO40_DMA_TX_RETURN
            const auto emitTransmitCosts=[&](const char *kind,unsigned port,unsigned ordinal,const DmaPreparation::TransmitReturn &s) {
                constexpr const char *names[]{"before-timer","ranges","modify","refresh-timer","cut-set","cut-notify","cut-timer","link-set","link-notify","after-timer"};
                for(unsigned phase=0;phase<s.phaseCalls.size();++phase)if(s.phaseCalls[phase])
                    std::fprintf(log,"transmit-phase kind=%s port=%u ordinal=%u phase=%s calls=%u wall-us=%llu cpu-ns=%llu\n",
                        kind,port,ordinal,names[phase],s.phaseCalls[phase],s.phaseMicros[phase],s.phaseCpuNS[phase]);
            };
#ifdef PRO40_DMA_TX_MEASURED
            for(unsigned stream=0;stream<2;++stream)if(firstPreparationMask[stream]) {
                const auto &cost=firstPreparationMicros[stream];
                std::fprintf(log,"first-transmit-preparation port=%u mask=%x ingest-us=%llu start-us=%llu produce-us=%llu\n",
                    stream+2,firstPreparationMask[stream],static_cast<unsigned long long>(cost[0]),
                    static_cast<unsigned long long>(cost[1]),static_cast<unsigned long long>(cost[2]));
            }
            if(firstGuardMeasured)std::fprintf(log,"first-transmit-guard us=%llu\n",static_cast<unsigned long long>(firstGuardMicros));
#endif
            for(unsigned stream=0;stream<2;++stream) {
                std::fprintf(log,"transmit-early-wait port=%u attempts=%u reads=%u max-us=%llu\n",stream+2,
                    earlyWaitCount[stream],earlyWaitReads[stream],static_cast<unsigned long long>(earlyWaitMaximum[stream]));
                std::fprintf(log,"transmit-early-revalidations port=%u count=%u\n",stream+2,earlyRevalidations[stream]);
                const auto &m=transmitMargins[stream];
                if(slowestTransmitReturns[stream].micros>=2000 || slowestTransmitReturns[stream].result)
                    emitTransmitCosts("slowest",stream+2,slowestTransmitOrdinals[stream],slowestTransmitReturns[stream]);
                std::fprintf(log,"transmit-margin port=%u attempted=%u completed=%u failed=%u min-before=%u before-ordinal=%u min-cut=%u cut-ordinal=%u min-after=%u after-ordinal=%u max-us=%llu slowest-ordinal=%u\n",
                    stream+2,m.attempted,m.completed,m.failed,m.minimumBefore,m.beforeOrdinal,
                    m.minimumCut,m.cutOrdinal,m.minimumAfter,m.afterOrdinal,
                    static_cast<unsigned long long>(m.maximumMicros),m.slowestOrdinal);
            }
            // Preserve first-bank native timings even when the rolling tail wraps.
            for(unsigned stream=0;stream<2;++stream)if(firstTransmitReturnMask&(1u<<stream)) {
                const auto &sample=firstTransmitReturns[stream];
                std::fprintf(log,"first-transmit-return port=%u before=%u cut-lead=%u after=%u elapsed-us=%llu status=%08x\n",
                    stream+2,sample.before,sample.cutLead,sample.after,
                    static_cast<unsigned long long>(sample.micros),unsigned(sample.result));
            }
#ifdef PRO40_DMA_TX_MEASURED
            emitMeasuredDiagnostics(log,measuredDiag,!options.warmupOnly);
#endif
            for(unsigned n=unsigned(txReturns.first());n<txReturns.end();++n) {
                [[maybe_unused]] const unsigned h=n/2;
                const unsigned stream=n%2;
                const auto &s=txReturns.read(n);
                if(s.result && (h!=slowestTransmitOrdinals[stream] || !s.micros))emitTransmitCosts("failed",stream+2,h,s);
                if(s.earlyRevalidated || s.result)std::fprintf(log,
                    "transmit-early-detail port=%u ordinal=%u reads=%u elapsed-us=%llu revalidated=%u result=%u lead=%u status=%08x\n",
                    stream+2,h,s.earlyReads,static_cast<unsigned long long>(s.earlyMicros),
                    unsigned(s.earlyRevalidated),unsigned(s.earlyResult),s.before,unsigned(s.result));
#ifdef PRO40_DMA_TX_REFRESH
                std::fprintf(log,"transmit-refresh port=%u packets=%u status=%08x\n",stream+2,s.refreshed,unsigned(s.refreshResult));
                if(h==0)dma[stream+2].logRefreshedPackets(refreshed[stream]);
#endif
                std::fprintf(log,"transmit-return port=%u callbacks=%u before=%u cut-lead=%u after=%u elapsed-us=%llu cut-status=%08x status=%08x\n",
                    stream+2,s.callbacks,s.before,s.cutLead,s.after,static_cast<unsigned long long>(s.micros),unsigned(s.cutResult),unsigned(s.result));
            }
#endif
#ifdef PRO40_DMA_RX_BRANCH_EXTEND
#ifdef PRO40_DMA_RX_HANDOFFS
            std::fprintf(log,"receive-handoff-policy version=2 independent=1\n");
            for(unsigned n=unsigned(extensions.first());n<extensionCount;++n) {
                const auto &record=extensions.read(n);
                const unsigned h=record.ordinal,stream=record.stream;
                const auto &e=record.sample;
                std::fprintf(log,"receive-bank-handoff port=%u ordinal=%u callbacks=%u before=%u cut-lead=%u after=%u elapsed-us=%llu cut-status=%08x status=%08x\n",
                    stream,h,e.callbacks,e.before,e.returnLead,e.after,static_cast<unsigned long long>(e.micros),unsigned(e.returnResult),unsigned(e.result));
                std::fprintf(log,"receive-bank-cost port=%u ordinal=%u mask=%02x before-timer=%llu cut-set=%llu cut-notify=%llu cut-timer=%llu link-set=%llu link-notify=%llu after-timer=%llu\n",
                    stream,h,e.phaseMask,static_cast<unsigned long long>(e.phaseMicros[0]),
                    static_cast<unsigned long long>(e.phaseMicros[1]),static_cast<unsigned long long>(e.phaseMicros[2]),
                    static_cast<unsigned long long>(e.phaseMicros[3]),static_cast<unsigned long long>(e.phaseMicros[4]),
                    static_cast<unsigned long long>(e.phaseMicros[5]),static_cast<unsigned long long>(e.phaseMicros[6]));
            }
#else
            for(unsigned stream=0;stream<2;++stream) {
                const auto &e=extensions[stream];
#ifdef PRO40_DMA_RX_DESCRIPTOR_REUSE
                std::fprintf(log,"receive-return-stop port=%u descriptor=%u lead=%u status=%08x\n",
                    stream,DmaPreparation::receiveCopyCount-1,e.returnLead,unsigned(e.returnResult));
#endif
                std::fprintf(log,"receive-branch-extend port=%u callbacks=%u before=%u after=%u elapsed-us=%llu status=%08x\n",
                    stream,e.callbacks,e.before,e.after,static_cast<unsigned long long>(e.micros),unsigned(e.result));
            }
#endif
#endif
#ifdef PRO40_DMA_TX_LOCK
            if(pendingPoll) {
                const auto &partial=pendingPoll->sample();
                std::fprintf(log,"refill-lock-pending state=%u error=%u start=%u attempted=%x valid=%x native=%08x transfer=%08x\n",
                    unsigned(pendingPoll->state()),unsigned(pendingPoll->error()),partial.started,
                    partial.attempted,partial.valid,unsigned(partial.ioStatus),unsigned(status));
                std::fprintf(log,"refill-lock-pending-values clock=%08x extended=%08x rate=%u cycle=%08x\n",
                    partial.values[0],partial.values[1],partial.values[2],partial.values[3]);
            }
            for(unsigned n=unsigned(refillLockSamples.first());n<refillLockCount;++n) {
                const auto &s=refillLockSamples.read(n);
                std::fprintf(log,"refill-lock progress=%u cycle=%08x clock=%08x extended=%08x rate=%u status=%08x\n",
                    s.progress,s.cycle,s.clock,s.extended,s.rate,unsigned(s.result));
                std::fprintf(log,"refill-lock-staged start=%u complete=%u iterations=%u,%u,%u,%u\n",
                    s.started,s.progress,s.iterations[0],s.iterations[1],s.iterations[2],s.iterations[3]);
                std::fprintf(log,"refill-lock-cost progress=%u mask=%x clock-us=%llu extended-us=%llu rate-us=%llu timer-us=%llu\n",
                    s.progress,s.measured,static_cast<unsigned long long>(s.micros[0]),
                    static_cast<unsigned long long>(s.micros[1]),static_cast<unsigned long long>(s.micros[2]),
                    static_cast<unsigned long long>(s.micros[3]));
            }
#endif
#ifdef PRO40_DMA_TX_MEASURED
            measuredDiag.emit(log,"measured");
#endif
#ifdef PRO40_DMA_TX_RETURN
            txReturns.emit(log,"tx-return");
#endif
#ifdef PRO40_DMA_RX_HANDOFFS
            extensions.emit(log,"rx-handoff");
#endif
#ifdef PRO40_DMA_TX_LOCK
            refillLockSamples.emit(log,"lock");
            if(options.quietStartupControl)
                std::fprintf(log,"quiet-control-profile schema=1 evidence=initial-lock-only entered=%u progress=%u lock-observations=%u completed-progress=%u active-route-reads=%u\n",
                    unsigned(!quietControl.pollingAllowed()),quietControl.enteredProgress(),refillLockCount,
                    completedProgress,activeRouteCosts[0].calls+activeRouteCosts[1].calls);
            std::fprintf(log,"playback-lock-summary first=%u last=%u locked=%u complete=%u accepted-at-stop=%u\n",
                refillLockCheck.firstLocked(),refillLockCheck.lastLocked(),refillLockCheck.lockedCount(),unsigned(refillLockCheck.complete()),unsigned(lockEvidenceComplete));
            std::fprintf(log,"playback-lock-gaps max-cycles=%u qualified-staged-extensions=%u\n",
                refillLockCheck.maximumAcceptedGap(),refillLockCheck.extendedStagedGaps());
#endif
            session.event("duplex-transfer",unsigned(status));
            std::fprintf(log,"duplex-dispatch loops=%u calls=%u callbacks=%u max-callbacks=%u total-us=%llu max-us=%llu\n",
                loopCount,dispatchCount,dispatchedCallbacks,maxDispatchCallbacks,
                static_cast<unsigned long long>(dispatchMicros),static_cast<unsigned long long>(maxDispatchMicros));
            std::fprintf(log,"duplex-dispatch-policy one-source=1 slowest-cpu-ns=%llu delayed=%u\n",
                (unsigned long long)slowestDispatchCpuNS,dispatchDelayCount);
            for(unsigned n=dispatchDelayCount>dispatchDelays.size()?dispatchDelayCount-unsigned(dispatchDelays.size()):0;n<dispatchDelayCount;++n) {
                const auto &d=dispatchDelays[n%dispatchDelays.size()];
                std::fprintf(log,"dispatch-delay iteration=%u wall-us=%llu cpu-ns=%llu callbacks=%u\n",d.iteration,
                    (unsigned long long)d.wallMicros,(unsigned long long)d.cpuNS,d.callbacks);
#ifdef PRO40_DUPLEX_WORKER
                const auto &w=d.waiting;
                std::fprintf(log,"dispatch-wait iteration=%u valid=%u waits=%u wait-wall-us=%llu wait-cpu-ns=%llu outside-wall-us=%llu outside-cpu-ns=%llu\n",
                    d.iteration,unsigned(w.valid),w.waits,(unsigned long long)(w.waitWall/1000),(unsigned long long)w.waitCPU,
                    (unsigned long long)(w.valid?(w.wall-w.waitWall)/1000:0),(unsigned long long)(w.valid?w.cpu-w.waitCPU:0));
#endif
            }
            generationCost.emit(log,"generation");
#ifdef PRO40_DMA_SOURCE_QUEUE
            for(unsigned p=0;p<4;++p)
                std::fprintf(log,"dispatch-boundary port=%u yields=%u active-at-stop=%u\n",p,dma[p].dispatchYields(),unsigned(dma[p].dispatchActive()));
#endif
#ifdef PRO40_DMA_RX_HANDOFFS
            receiveGuardCost.emit(log,"receive-generation-guard");
#endif
#ifdef PRO40_DMA_DUPLEX_REPLAY
            ownerCost.emit(log,"owner");composeCost.emit(log,"compose");
            publishCost.emit(log,"publish");lockCost.emit(log,"lock");
            for(unsigned i=0;i<backlogCount;++i) {
                const auto &b=backlogs[i];
                std::fprintf(log,"replay-backlog offset=%u rx0=%u rx1=%u\n",b.offset,b.rx0,b.rx1);
            }
            for(unsigned i=0;i<lockCount;++i) {
                const auto &s=lockSamples[i];
                std::fprintf(log,"replay-lock progress=%u cycle=%08x clock=%08x extended=%08x rate=%u status=%08x\n",
                    s.progress,s.cycle,s.status,s.extended,s.rate,unsigned(s.result));
            }
            for(unsigned i=0;i<updateCount;++i) {
                const auto &u=updates[i];
                std::fprintf(log,"replay-update port=%u first=%u before=%u after=%u status=%08x\n",
                    u.port,u.first,u.before,u.after,unsigned(u.result));
                std::fprintf(log,"replay-queue port=%u first=%u before=%u after=%u limit=%u before-status=%u after-status=%u publish-us=%llu\n",
                    u.port,u.first,u.queueBefore.info.mps_msgcount,u.queueAfter.info.mps_msgcount,
                    u.queueBefore.info.mps_qlimit,unsigned(u.queueBefore.result),unsigned(u.queueAfter.result),
                    static_cast<unsigned long long>(u.publishMicros));
                std::fprintf(log,"replay-native-cost port=%u first=%u ranges-us=%llu notify-us=%llu notify-status=%08x\n",
                    u.port,u.first,static_cast<unsigned long long>(u.timing.rangesMicros),
                    static_cast<unsigned long long>(u.timing.notifyMicros),unsigned(u.timing.notifyResult));
            }
            // Save the exact composition buffers supplied to native Notify.
            // These are prepared host bytes, not an independent wire capture.
            for(unsigned stream=0;stream<2;++stream)
                for(const auto &packet:prepared[stream]) {
                    std::array<uint8_t,4> length{};
                    for(unsigned b=0;b<4;++b)length[b]=uint8_t(packet.length>>(b*8));
                    if(std::fwrite(length.data(),1,4,captures[2+stream])!=4 ||
                        std::fwrite(packet.bytes.data(),1,448,captures[2+stream])!=448)clean(kIOReturnIOError);
                }
#endif
            for(unsigned p=0;p<dma.size();++p) {
                const auto result=dma[p].releaseDetailed((recoveryRequested && !status && !cleanup) || options.sustained);
                clean(result.native);
                if(result.evidence && !evidence)evidence=result.evidence;
                std::fprintf(log,"dma-release port=%u native=%08x evidence=%08x\n",p,
                    unsigned(result.native),unsigned(result.evidence));
            }
            // Linux dice-interface.h:119-125: ENABLE0 stops all streams even
            // while ISOCHRONOUS assignments remain set. Keep reservations held.
            if(options.assignedIdleObservation){
                observeIdle(session.owner);
                if(quietStarted && quietState==PreCloseQuietHold::State::complete)
                    assignedGuard=inputGuard(session.owner);
                restoreBegin=session.startupTrace.now();restoreHostBegin=mach_absolute_time();
            }
            clean(config.restore());
            if(options.assignedIdleObservation){
                restoreHostEnd=mach_absolute_time();restoreEnd=session.startupTrace.now();
            }
            session.event("duplex-channels-restored",unsigned(cleanup));
            if(!cleanup && !prelude)inputBoundary(Pro40::InputBoundaryTrace::Stage::channelsRestored,session.owner);
            for(auto it=reservations.rbegin();it!=reservations.rend();++it)clean(it->release());
        }
        if(haveBefore) {
            auto result=Pro40Resources::snapshot(device,session.generation,after);
            if(!result && (after.bandwidth!=before.bandwidth || after.channels!=before.channels))result=kIOReturnError;
            clean(result);session.event("duplex-IRM-restored",unsigned(result));
        }
        if(dispatcher)(*device)->RemoveIsochCallbackDispatcherFromRunLoop(device);
        // A sibling's finalization can dispatch a late completion for a port
        // already released. Recheck every port after the dispatcher is removed.
        for(const auto &port:*dmaStorage)clean(port.callbackStatus());
        if(!prelude)break;
        std::fprintf(log,"startup-recovery-boundary requested=%u transfer=%08x evidence=%08x cleanup=%08x\n",
            unsigned(recoveryRequested),unsigned(status),unsigned(evidence),unsigned(cleanup));
        // Native ports and IRM allocations are released before a fresh attempt.
        // Keep old mappings/refcons until native close: re-adding the shared
        // dispatcher must never expose a dangling callback from attempt zero.
        // Keep the DICE notification owner and settled clock, as
        // DiceFamilyDriver.cpp:444-469,1023-1027 at PR180's merge commit do.
        if(!recoveryRequested || status || evidence || cleanup)break;
        if(stop.requested()){status=kIOReturnAborted;stopReason=StopReason::interrupted;break;}
        if(!lifecycle.finish(evidence,cleanup) || !lifecycle.start()){status=kIOReturnError;break;}
        recoveryRequested=false;
        retiredDmaStorage=std::move(dmaStorage);
        dmaStorage.reset(new(std::nothrow) DmaStorage);
        if(!dmaStorage){status=kIOReturnNoMemory;break;}
        status=session.verifyExisting48k([&]{return stop.requested();});
        session.event("startup-recovery-retained-clock",unsigned(status));
        session.startupTrace.phase(StartupNotificationTrace::Phase::preparation);
        options=requestedOptions;
    }
    if(lifecycle.phase()!=Pro40::SessionPhase::stopping) {
        if(status==kIOReturnAborted) {
            (void)lifecycle.requestStop();stopReason=Pro40::StopReason::interrupted;
        }
        if(!lifecycle.beginStop(stopReason,status))clean(kIOReturnError);
    }
    if(session.device)pro40_begin_cleanup();
    session.startupTrace.phase(Pro40::StartupNotificationTrace::Phase::cleanup);
    IOReturn ownedGuard=kIOReturnNotReady;
    if(options.ownedIdleObservation){
        if(!options.assignedIdleObservation)observeIdle(session.owner);
        if(quietStarted && quietState==PreCloseQuietHold::State::complete)
            ownedGuard=inputGuard(session.owner);
    }
    const auto releaseBegin=options.ownedIdleObservation?session.startupTrace.now():0;
    const auto releaseHostBegin=options.ownedIdleObservation?mach_absolute_time():0;
    clean(session.finish());
    const auto releaseHostEnd=options.ownedIdleObservation?mach_absolute_time():0;
    const auto releaseEnd=options.ownedIdleObservation?session.startupTrace.now():0;
    if(!cleanup && session.claimAttempted)inputBoundary(Pro40::InputBoundaryTrace::Stage::ownerReleased,session.noOwner);
    std::fprintf(log,"duplex-notifications count=%u bits=%08x invalid=%u\n",
        session.notifications,session.bits,session.invalidPacket);
    if(!options.ownedIdleObservation)observeIdle(session.noOwner);
    const auto closeBegin=options.postReleasePeaks?session.startupTrace.now():0;
    const auto closeHostBegin=options.preCloseQuietHold?mach_absolute_time():0;
    pro40_close();const auto nativeClosedAt=ShutdownClock::now();
    const auto closeHostEnd=options.preCloseQuietHold?mach_absolute_time():0;
    const auto closeEnd=options.postReleasePeaks?session.startupTrace.now():0;
    for(auto *storage:{dmaStorage.get(),retiredDmaStorage.get()})if(storage)
        for(auto &port:*storage)clean(port.reclaimAfterNativeClose());
    session.event("duplex-closed");
    if(options.postReleasePeaks)
        std::fprintf(log,"native-close-bracket schema=1 begin-us=%llu end-us=%llu\n",
            static_cast<unsigned long long>(closeBegin),static_cast<unsigned long long>(closeEnd));
    session.startupTrace.dump(log);
    inputBoundaries.dump(log);
    if(options.postReleasePeaks){
        std::fprintf(log,"%s schema=1 started=%u state=%u origin-us=%llu samples=%u\n",
            options.assignedIdleObservation?"assigned-idle-window":options.ownedIdleObservation?"owned-idle-window":"post-release-window",unsigned(postReleaseStarted),unsigned(postRelease.state()),static_cast<unsigned long long>(postReleaseOrigin),postRelease.size());
        for(unsigned i=0;i<postRelease.size();++i){const auto &r=postRelease[i];
            std::fprintf(log,"%s index=%u begin-us=%llu end-us=%llu valid=%u io=%08x error=%u raw=%08x peak=%u\n",
                options.assignedIdleObservation?"assigned-idle-peak":options.ownedIdleObservation?"owned-idle-peak":"post-release-peak",i,static_cast<unsigned long long>(r.begin),static_cast<unsigned long long>(r.end),unsigned(r.peak.valid),
                unsigned(r.peak.ioStatus),r.peak.error,r.peak.raw,r.peak.peak);
        }
    }
    if(options.preCloseQuietHold)
        std::fprintf(log,"%s schema=1 started=%u state=%u cleanup-origin-us=%llu begin-us=%llu end-us=%llu\n",
            options.assignedIdleObservation?"assigned-idle-quiet-hold":options.ownedIdleObservation?"owned-idle-quiet-hold":"pre-close-quiet-hold",unsigned(quietStarted),unsigned(quietState),static_cast<unsigned long long>(cleanupOrigin),
            static_cast<unsigned long long>(quietBegin),static_cast<unsigned long long>(quietEnd));
    if(options.preCloseQuietHold)
        std::fprintf(log,"native-close-host schema=1 begin-ticks=%llu end-ticks=%llu\n",
            static_cast<unsigned long long>(closeHostBegin),static_cast<unsigned long long>(closeHostEnd));
    if(options.ownedIdleObservation)
        std::fprintf(log,"owner-release-wrapper schema=1 begin-us=%llu end-us=%llu begin-ticks=%llu end-ticks=%llu guard=%08x\n",
            static_cast<unsigned long long>(releaseBegin),static_cast<unsigned long long>(releaseEnd),
            static_cast<unsigned long long>(releaseHostBegin),static_cast<unsigned long long>(releaseHostEnd),unsigned(ownedGuard));
    if(options.assignedIdleObservation)
        std::fprintf(log,"channel-restore-wrapper schema=1 begin-us=%llu end-us=%llu begin-ticks=%llu end-ticks=%llu guard=%08x\n",
            static_cast<unsigned long long>(restoreBegin),static_cast<unsigned long long>(restoreEnd),
            static_cast<unsigned long long>(restoreHostBegin),static_cast<unsigned long long>(restoreHostEnd),unsigned(assignedGuard));
    for(auto f:captures)if(f && std::fclose(f))clean(kIOReturnIOError);
    NativeClockEvidenceAge clockAge;
    uint64_t evidenceBefore=0,evidenceAfter=0,completionHost=0;
    uint32_t evidenceNumer=0,evidenceDenom=0;
    if(options.timing&&options.timing->used){
        const auto &timing=*options.timing;
        const bool countOK=timing.used<=timing.clocks.size();
        if(countOK){evidenceBefore=timing.clocks[timing.used-1].before;evidenceAfter=timing.clocks[timing.used-1].after;}
        evidenceNumer=timing.numer;evidenceDenom=timing.denom;
        mach_timebase_info_data_t host{};
        const auto result=mach_timebase_info(&host);
        completionHost=mach_absolute_time();
        clockAge=NativeClockEvidenceAge::measure(true,countOK&&timing.valid()&&result==KERN_SUCCESS,
            evidenceBefore,evidenceAfter,completionHost,evidenceNumer,evidenceDenom,host.numer,host.denom);
    }
    const auto completionBegin=ShutdownClock::now();
    int32_t completionResult=0;
    if(options.sourceCompletion.finish){
        const bool accepted=stopReason==StopReason::completed&&!status&&!evidence&&!cleanup;
        completionResult=options.sourceCompletion.status(accepted,stop);
        if(completionResult&&!evidence)evidence=completionResult;
    }
    const auto completionEnd=ShutdownClock::now();
    if(options.sourceCompletion.finish)session.event("duplex-source-completed",unsigned(completionResult));
    // Native DMA and mappings are gone. Peer supervision must use only owned
    // copies; its failure feeds the same admission/lease cleanup gate.
    const auto peerCleanupBegin=ShutdownClock::now();
    clean(options.peerCleanup.status());
    const auto peerCleanupEnd=ShutdownClock::now();
    auto micros=[](auto first,auto last){return static_cast<unsigned long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(last-first).count());};
    // Emit only after drain/cleanup; no added IO in capture or timed regions.
    std::fprintf(log,"duplex-shutdown-timing capture-exit=%u source-hook=%u capture-to-source-us=%llu native-close-to-source-us=%llu source-us=%llu peer-cleanup-us=%llu\n",
        unsigned(captureExit.has_value()),unsigned(options.sourceCompletion.finish!=nullptr),
        captureExit?micros(*captureExit,completionBegin):0,micros(nativeClosedAt,completionBegin),
        micros(completionBegin,completionEnd),micros(peerCleanupBegin,peerCleanupEnd));
    std::fprintf(log,"duplex-clock-evidence-age state=%u lower-us=%llu upper-us=%llu before=%llu after=%llu completion-host=%llu numer=%u denom=%u\n",
        unsigned(clockAge.state),static_cast<unsigned long long>(clockAge.lowerMicros),
        static_cast<unsigned long long>(clockAge.upperMicros),static_cast<unsigned long long>(evidenceBefore),
        static_cast<unsigned long long>(evidenceAfter),static_cast<unsigned long long>(completionHost),evidenceNumer,evidenceDenom);
    if(!lifecycle.finish(evidence,cleanup))clean(kIOReturnError);
    auto outcome=lifecycle.outcome();
    session.event("duplex-cleanup",unsigned(cleanup));
    session.event("duplex-evidence",unsigned(evidence));
    std::fprintf(log,"duplex-result reason=%s transfer=%08x evidence=%08x cleanup=%08x\n",
        Pro40::name(stopReason),unsigned(status),unsigned(evidence),unsigned(cleanup));
    std::fprintf(log,"session-lifecycle closed=%u accepted=%u\n",unsigned(outcome.has_value()),unsigned(outcome && outcome->accepted()));
    if(std::fclose(log))clean(kIOReturnIOError);
    const auto result=closedSessionResult(outcome,status,evidence,cleanup);
    if(cleanup || (result.outcome && result.outcome->cleanup))admission.blockRestart();
    return result;
}
