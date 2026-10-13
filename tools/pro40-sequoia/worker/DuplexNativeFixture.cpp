// SPDX-License-Identifier: Apache-2.0
// Test-only replacement for the native runner. Links NO native FireWire code.
// Actual NativeWorker, managed admission, lease and duplex bridge remain in use.
#include "../NativeSessionRunner.hpp"
#include "../Pro40NativeReplay.hpp"
#include "../PlaybackStartupGate.hpp"
#include "../NativeInputFixture.hpp"
#include "../NativeInputDelivery.hpp"
#include "../NativeDuplexPolicy.hpp"
#include <IOKit/IOReturn.h>
#include <mach/mach_time.h>
#include <chrono>
#include <thread>
#include <cassert>
#include <cstdio>
namespace Pro40 {
NativeSessionResult runNativeSession(const NativeSessionOptions &options) {
    using namespace Pro40Audio;using namespace std::chrono_literals;using Wall=std::chrono::steady_clock;
    assert(options.sustained && options.inputHandoff && options.livePlayback && options.inputMonitor && !options.playback);
    FILE *log=std::fopen(options.logPath.c_str(),"wx");assert(log);std::fputs("generated-native-entry\n",log);std::fflush(log);
    mach_timebase_info_data_t tb{};assert(mach_timebase_info(&tb)==KERN_SUCCESS);
    const auto ticks=[&](uint64_t ns){return uint64_t(__uint128_t(ns)*tb.denom/tb.numer);};
    // Independent64-packet receive publications; playback consumes older
    //256-packet groups. Model the unchanged native presentation phase separately.
    const auto origin=mach_absolute_time();
    auto native=std::make_unique<Fixture::NativeSource>();auto &packets=native->packets;
    auto pair=[&](bool anchor=false){
        auto p=packets.clock(anchor);const auto value=origin+uint64_t(__uint128_t(p.uptime-Fixture::Source::hostOrigin)*125*tb.denom/(3*tb.numer));
        p.uptime=value;p.before=value-ticks(3000);p.after=value+ticks(3000);return p;
    };
    assert(options.inputHandoff->clock(pair(true)));
    Pro40Packets::NativeReplay replay(0,4,3,1,7,nullptr,nullptr,false,nullptr,options.livePlayback);
    NativeInputDelivery inputDelivery(4,3,7,*options.inputMonitor,options.inputHandoff);
    auto &gate=*static_cast<PlaybackStartupGate *>(options.livePlayback->context);
    std::array<uint8_t,448> output{};uint64_t prepared=0,audio=0;uint32_t cycle=918;
#ifdef PRO40_DELEGATED_FIXTURE
    uint64_t signalFrames=0,silentFrames=0;
#endif
    NativePackets::PreparedBuffer input;uint32_t status=0,stamp=0;
    const auto began=Wall::now();auto next=mach_absolute_time()+ticks(8000000);
    // Session lifecycle qualification deliberately exceeds the old 30s owner
    // limit. Keep an independent fixture deadline so a broken Stop still fails.
#ifdef PRO40_SESSION_PROFILE
    const auto deadline=began+90s;
#else
    const auto deadline=began+5s;
#endif
    unsigned groups=0;int64_t maxLate=0;
    while(!options.stop.requested() && Wall::now()<deadline) {
        // Keep servicing cancellation at1ms intervals, like the native
        // runloop, while preserving the8ms receive-publication deadline. Both long
        // relative and absolute waits woke>70ms late in the background fixture.
        // This is synthetic-source pacing only; real packets arrive via DMA.
        for(auto now=mach_absolute_time();now<next && !options.stop.requested();now=mach_absolute_time())
            assert(mach_wait_until(std::min(next,now+ticks(1000000)))==KERN_SUCCESS);
        const auto woke=mach_absolute_time();
        if(woke>next)maxLate=std::max(maxLate,int64_t(__uint128_t(woke-next)*tb.numer/tb.denom));
        next+=ticks(8000000);
        if(options.stop.requested())break;
        bool nativeFault=false;
        native->prepare();assert(native->append());
        if(!inputDelivery.drain(native->queue.next(),7,[&](uint64_t n,auto &b,auto &s,auto &t) {
            return native->queue.observe(n,7,b,s,t);
        }))break;
        if(!options.inputHandoff->clock(pair()))break;
        const auto first=native->queue.copied();
        if(native->queue.next()-first<512)continue;
        for(unsigned n=0;n<256;++n) {
            assert(native->queue.copy(first+n,7,input,status,stamp));
            if(replay.ingest(input.bytes,status,stamp,7)!=Pro40Packets::NativeReplayError::none){nativeFault=true;break;}
        }
        if(nativeFault)break;
        if(!groups)assert(replay.start(cycle,7)==Pro40Packets::ReplayError::none);
        for(unsigned n=0;n<256;++n) {
            const auto made=replay.produce(output,cycle,7);if(!made){nativeFault=true;break;}
            const auto parsed=Pro40Packets::parse(std::span(output).first(made.bytes),Pro40Packets::playback[0],1);assert(parsed);
            for(unsigned f=0;f<parsed.packet.blocks;++f)for(unsigned c=0;c<13;++c) {
                const auto boundary=gate.firstNativeFrame();const auto frame=prepared+f;
                const bool app=boundary!=UINT64_MAX && frame>=boundary+nativeDuplexPrimeFrames;
#ifdef PRO40_DELEGATED_FIXTURE
                // Separate generated HAL client sends a channel-specific DC
                // marker. Entire silent frames are permitted around app start/
                // stop and in the explicit first-callback gap; a mixed/misrouted
                // frame still fails. The collector requires substantial signal.
                const bool signal=app && Pro40Packets::read32(output.data()+8+f*13*4)!=0x40000000;
                const int32_t sample=signal && c<6?int32_t(100000*(c+1)):0;
                if(app && c==0){if(signal)++signalFrames;else ++silentFrames;}
#else
                const int32_t sample=app && c<6?Fixture::sample(frame-boundary,c):0;
#endif
                const uint32_t expected=c==12?0x80000000:0x40000000|(uint32_t(sample)&0xffffff);
                assert(Pro40Packets::read32(output.data()+8+(f*13+c)*4)==expected);
                if(app && c==0)++audio;
            }
            prepared+=parsed.packet.blocks;cycle=(cycle+1)%64000;
        }
        if(nativeFault)break; // No invented clock/packet progress after native replay stops.
        assert(native->queue.acknowledge(first,7,256));
        ++groups;
    }
    assert(native->queue.observerValid() && inputDelivery.next()>native->queue.copied());
    std::fprintf(log,"early-input packets=%llu replay=%llu max-lead=%llu max-batch=%u fault=%u\n",
        (unsigned long long)inputDelivery.next(),(unsigned long long)native->queue.copied(),
        (unsigned long long)native->queue.maximumObserverLead(),inputDelivery.maximumBatch(),unsigned(inputDelivery.fault()));
    assert(Wall::now()<deadline); // Parent/bridge must stop; the fixture never self-reports success on timeout.
    const bool dirty=std::getenv("PRO40_DUPLEX_FIXTURE_DIRTY")!=nullptr;
    const bool underrun=std::getenv("PRO40_DUPLEX_FIXTURE_UNDERRUN")!=nullptr;
    std::fprintf(log,"prepared=%llu application=%llu cutover=%llu started=%llu\n",(unsigned long long)prepared,
        (unsigned long long)audio,(unsigned long long)gate.firstNativeFrame(),(unsigned long long)gate.startedAt());
    std::fprintf(log,"elapsed_ns=%lld max_late_ns=%lld\n",(long long)std::chrono::duration_cast<std::chrono::nanoseconds>(Wall::now()-began).count(),(long long)maxLate);
#ifdef PRO40_DELEGATED_FIXTURE
    std::fprintf(log,"delegated-output signal-frames=%llu silent-frames=%llu\n",(unsigned long long)signalFrames,(unsigned long long)silentFrames);
#endif
    // The parent validates admitted audio length; allow startup faults to return
    // through normal teardown so the bridge report preserves their exact cause.
    const bool gateOK=gate.fault()==(underrun?PlaybackStartupFault::source:PlaybackStartupFault::none) &&
        gate.startedAt()==gate.firstNativeFrame();
    // Simulate asynchronous native close latency. STOPPED must wait for return.
    std::this_thread::sleep_for(40ms);std::fputs("generated-native-closed\n",log);assert(std::fclose(log)==0);
    SessionOutcome outcome;outcome.reason=StopReason::interrupted;outcome.transfer=kIOReturnAborted;
    outcome.cleanup=dirty?kIOReturnIOError:0;outcome.evidence=gateOK?0:kIOReturnError;return {2,outcome};
}
}
