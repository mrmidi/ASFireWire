// SPDX-License-Identifier: Apache-2.0
// Link the real runner to fail-fast native stubs; this executable cannot access
// FireWire hardware. Any unexpected call into that boundary terminates the test.
#include "NativeSessionRunner.hpp"
#include "NativePeerSupervisor.hpp"
#include "NativeTimingEvidence.hpp"
#include "InputRecording.hpp"
#include "PlaybackClip.hpp"
#include "NativeFireWire.h"
#include <IOKit/IOReturn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>
static bool allowOpenFailure=false,nativeClosed=false;
static unsigned opens=0,closes=0;
static bool generationMismatch=false;
extern "C" {
int32_t pro40_open(uint64_t){assert(allowOpenFailure);++opens;nativeClosed=false;return generationMismatch?0:kIOReturnNoDevice;}
int32_t pro40_state(uint32_t *generation,uint16_t *node){assert(generationMismatch);*generation=8;*node=0;return 0;}
int32_t pro40_admit_read(uint64_t,uint32_t){std::abort();}
int32_t pro40_read(uint32_t,uint64_t,uint32_t,void *){std::abort();}
void pro40_begin_cleanup(){std::abort();}
void pro40_close(){assert(allowOpenFailure);++closes;nativeClosed=true;}
int32_t pro40_clock_state(uint32_t,Pro40ClockState *){std::abort();}
void *pro40_native_interface(){assert(generationMismatch);return nullptr;}
}
struct StopState {
    bool stop=true;
    unsigned polls=0;
    static bool poll(void *context) noexcept {
        auto &self=*static_cast<StopState *>(context);++self.polls;return self.stop;
    }
};
struct PeerState {
    unsigned calls=0;bool confirmed=true;
    struct Resources {
        PeerState &peer;unsigned step=0;
        bool quiesceAndJoin(){assert(nativeClosed&&step++==0);return true;}
        bool senderClosed(){assert(step++==1);return true;}
        bool confirmRemote(){assert(step++==2);return peer.confirmed;}
        bool closeControl(){assert(step++==3);return true;}
    } resources{*this};
    Pro40Audio::NativePeerSupervisor<Resources> supervisor{resources};
    static int32_t finish(void *context) noexcept {
        auto &p=*static_cast<PeerState *>(context);assert(nativeClosed);++p.calls;
        return p.supervisor.finish();
    }
};
int main() {
    namespace fs=std::filesystem;
    char pattern[]="build/pro40-sequoia/entry-check-XXXXXX";
    const auto directory=mkdtemp(pattern);assert(directory);
    const fs::path root(directory);
    StopState stop;
    Pro40::NativeSessionOptions options{0x00130e0400000001,(root/"fresh.log").string(),false,
        {StopState::poll,&stop}};
    PeerState peer;options.peerCleanup={PeerState::finish,&peer};
    for(unsigned i=0;i<100;++i) {
        const auto result=Pro40::runNativeSession(options);
        assert(result.exitCode==2 && !result.accepted() && result.outcome);
        assert(result.outcome->reason==Pro40::StopReason::interrupted);
        assert(result.outcome->transfer==kIOReturnAborted);
        assert(result.outcome->evidence==0 && result.outcome->cleanup==0);
        assert(fs::is_empty(root)); // No log or packet exports were opened.
    }
    assert(stop.polls==100);
    auto invalid=options;invalid.logPath.clear();
    const auto badPath=Pro40::runNativeSession(invalid);
    assert(badPath.exitCode==64 && !badPath.outcome && stop.polls==100);
    invalid=options;invalid.retainHistory=true; // Unsupported for65536cycle profile.
    const auto badHistory=Pro40::runNativeSession(invalid);
    assert(badHistory.exitCode==64 && !badHistory.outcome && stop.polls==100);
    auto timing=std::make_unique<Pro40::NativeTimingEvidence>();
    {
        Pro40Packets::InputRecording eight(8),invalidRecording(2);
        auto bad=options;bad.recording=&invalidRecording;
        assert(Pro40::runNativeSession(bad).exitCode==64);
        bad.recording=&eight;bad.timing=timing.get();
        assert(Pro40::runNativeSession(bad).exitCode==64); // Mono timing bridge only.
        std::array<uint8_t,18> pcm{};
        const Pro40Packets::PlaybackClip clip{pcm,6},invalidClip{pcm,13};
        bad=options;bad.playback=&clip; // Existing peer hook is incompatible.
        assert(Pro40::runNativeSession(bad).exitCode==64);
        bad.peerCleanup={};bad.playback=&invalidClip;
        assert(Pro40::runNativeSession(bad).exitCode==64);
        bad.playback=&clip;bad.warmupOnly=true;
        assert(Pro40::runNativeSession(bad).exitCode==64);
        assert(stop.polls==100 && fs::is_empty(root));
        // Valid duplex is admitted to cancellation without touching hardware.
        StopState duplexStop;
        bad=options;bad.peerCleanup={};bad.recording=&eight;bad.playback=&clip;
        bad.stop={StopState::poll,&duplexStop};
        assert(Pro40::runNativeSession(bad).outcome->reason==Pro40::StopReason::interrupted);
        assert(duplexStop.polls==1 && fs::is_empty(root));
    }
    assert(timing->attachConsumer({nullptr,
        [](void *,size_t,const Pro40Packets::InputTimingEvidence<>::Record &) noexcept {std::abort();return false;},
        [](void *,const Pro40::NativeTimingEvidence::Clock &) noexcept {std::abort();return false;},
        [](void *) noexcept {std::abort();return false;}}));
    invalid=options;invalid.timing=timing.get(); // Active consumer requires recorder PCM.
    const auto missingRecorder=Pro40::runNativeSession(invalid);
    assert(missingRecorder.exitCode==64&&!missingRecorder.outcome&&stop.polls==100);
    invalid=options;invalid.verifyExistingClockOnly=true;
    assert(Pro40::runNativeSession(invalid).exitCode==64);
    invalid=options;invalid.assignedIdleObservation=true;
    assert(Pro40::runNativeSession(invalid).exitCode==64);
    invalid=options;invalid.ownedIdleObservation=true;
    assert(Pro40::runNativeSession(invalid).exitCode==64);
    invalid=options;invalid.preCloseQuietHold=true;
    assert(Pro40::runNativeSession(invalid).exitCode==64);
    invalid=options;invalid.postReleasePeaks=true;
    const auto invalidPostRelease=Pro40::runNativeSession(invalid);
    assert(invalidPostRelease.exitCode==64&&!invalidPostRelease.outcome&&stop.polls==100);
    StopState warmupStop;
    auto warmup=options;warmup.peerCleanup={};warmup.warmupOnly=true;warmup.stop={StopState::poll,&warmupStop};
    warmup.enableFirstStartup=true;warmup.assignedIdleObservation=true;
    warmup.ownedIdleObservation=true;warmup.preCloseQuietHold=true;warmup.postReleasePeaks=true;
    for(unsigned fault=0;fault<10;++fault) {
        auto bad=warmup;
        switch(fault) {
        case 0:bad.retainHistory=true;break;
        case 1:bad.timing=timing.get();break;
        case 2:bad.clockExcursion=true;break;
        case 3:bad.verifyExistingClockOnly=true;break;
        case 4:bad.enableFirstStartup=false;break;
        case 5:bad.assignedIdleObservation=false;break;
        case 6:bad.ownedIdleObservation=false;break;
        case 7:bad.preCloseQuietHold=false;break;
        case 8:bad.postReleasePeaks=false;break;
        case 9:bad.peerCleanup=options.peerCleanup;break;
        }
        assert(Pro40::runNativeSession(bad).exitCode==64 && stop.polls==100 && warmupStop.polls==0);
        assert(fs::is_empty(root));
    }
    // The valid warm-up reaches cancellation, with no native entry or PCM file.
    assert(Pro40::runNativeSession(warmup).outcome->reason==Pro40::StopReason::interrupted);
    assert(fs::is_empty(root) && warmupStop.polls==1);
    auto recovery=warmup;recovery.warmupOnly=false;recovery.startupRecovery=true;
    auto input=std::make_unique<Pro40Packets::InputRecording>();
    auto passiveTiming=std::make_unique<Pro40::NativeTimingEvidence>();
    recovery.recording=input.get();recovery.timing=passiveTiming.get();
    for(unsigned fault=0;fault<9;++fault) {
        auto bad=recovery;
        switch(fault) {
        case 0:bad.recording=nullptr;break;
        case 1:bad.timing=nullptr;break;
        case 2:bad.warmupOnly=true;break;
        case 3:bad.clockExcursion=true;break;
        case 4:bad.verifyExistingClockOnly=true;break;
        case 5:bad.enableFirstStartup=false;break;
        case 6:bad.retainHistory=true;break;
        case 7:bad.peerCleanup=options.peerCleanup;break;
        case 8:bad.timing=timing.get();break;
        }
        assert(Pro40::runNativeSession(bad).exitCode==64 && warmupStop.polls==1);
        assert(fs::is_empty(root));
    }
#ifdef PRO40_STARTUP_RECOVERY
    assert(Pro40::runNativeSession(recovery).outcome->reason==Pro40::StopReason::interrupted);
    assert(warmupStop.polls==2 && fs::is_empty(root));
#else
    assert(Pro40::runNativeSession(recovery).exitCode==64 && warmupStop.polls==1);
#endif
    // Cancellation takes precedence over opening an existing log path.
    const auto sentinel=root/"existing.log";
    {std::ofstream file(sentinel);file<<"unchanged";}
    options.logPath=sentinel.string();
    const auto canceled=Pro40::runNativeSession(options);
    assert(canceled.exitCode==2 && canceled.outcome && stop.polls==101);
    // Clearing stop reaches exclusive-create failure, proving the prior lease
    // was released without needing to open hardware or use a fake success path.
    stop.stop=false;
    const auto prepare=Pro40::runNativeSession(options);
    assert(prepare.exitCode==1 && !prepare.outcome && stop.polls==102);
    std::ifstream file(sentinel);std::string contents;file>>contents;file.close();
    assert(contents=="unchanged");
    stop.stop=true;
    assert(Pro40::runNativeSession(options).outcome->reason==Pro40::StopReason::interrupted);
    assert(peer.calls==0&&opens==0&&closes==0);
    // Exercise actual runner finalization after a stubbed native-open failure.
    // No FireWire interface is ever supplied and all native reads still abort.
    stop.stop=false;allowOpenFailure=true;
    auto noPeer=options;noPeer.peerCleanup={};noPeer.logPath=(root/"no-peer.log").string();
    const auto defaultResult=Pro40::runNativeSession(noPeer);
    assert(defaultResult.exitCode==2&&defaultResult.outcome&&defaultResult.outcome->cleanup==0&&peer.calls==0&&closes==1);
    options.logPath=(root/"clean-peer.log").string();
    assert(peer.supervisor.arm());
    const auto clean=Pro40::runNativeSession(options);
    assert(clean.exitCode==2&&clean.outcome&&clean.outcome->cleanup==0&&peer.calls==1&&closes==2);
    generationMismatch=true;
    auto stale=noPeer;stale.expectedGeneration=7;stale.logPath=(root/"stale-generation.log").string();
    const auto refused=Pro40::runNativeSession(stale);
    assert(refused.exitCode==2 && refused.outcome && refused.outcome->transfer==kIOFireWireBusReset && refused.outcome->cleanup==0 && closes==3);
    generationMismatch=false;
    PeerState unknownPeer;unknownPeer.confirmed=false;assert(unknownPeer.supervisor.arm());
    options.peerCleanup={PeerState::finish,&unknownPeer};options.logPath=(root/"unknown-peer.log").string();
    const auto unknown=Pro40::runNativeSession(options);
    assert(unknown.exitCode==2&&unknown.outcome&&unknown.outcome->cleanup==-2&&peer.calls==1&&unknownPeer.calls==1&&closes==4);
    options.logPath=(root/"blocked.log").string();
    const auto blocked=Pro40::runNativeSession(options);
    assert(blocked.exitCode==75&&!blocked.outcome&&peer.calls==1&&unknownPeer.calls==1&&opens==4&&!fs::exists(options.logPath));
    fs::remove(root/"stale-generation.log");fs::remove(root/"no-peer.log");fs::remove(root/"clean-peer.log");fs::remove(root/"unknown-peer.log");fs::remove(sentinel);fs::remove(root);
    std::puts("PASS: real runner entry cancellation/preparation, post-native-close peer finalization, clean reuse and unconfirmed peer restart block; stubbed native boundary only");
}
