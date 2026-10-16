// SPDX-License-Identifier: Apache-2.0
#include "NativeSessionRunner.hpp"
#include "InputMonitor.hpp"
#include "SustainedInputHandoff.hpp"
#include "PlaybackPcmQueue.hpp"
#include "PlaybackClip.hpp"
#include "DmaPreparation.hpp"
#include <memory>
#include "InputRecording.hpp"
#include "NativeFireWire.h"
#include <IOKit/IOReturn.h>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

static unsigned opens=0,closes=0;
static_assert(DmaPreparation::receiveGate==512 && DmaPreparation::receiveCopyCount==256);
static_assert(DmaPreparation::transmitBankPackets==256 && DmaPreparation::transmitSlots==512);
static_assert(DmaPreparation::firstRefreshCycle==512);
extern "C" {
int32_t pro40_open(uint64_t){++opens;return kIOReturnNoDevice;}
void pro40_close(){++closes;}
int32_t pro40_state(uint32_t *,uint16_t *){std::abort();}
int32_t pro40_admit_read(uint64_t,uint32_t){std::abort();}
int32_t pro40_read(uint32_t,uint64_t,uint32_t,void *){std::abort();}
void pro40_begin_cleanup(){std::abort();}
int32_t pro40_clock_state(uint32_t,Pro40ClockState *){std::abort();}
void *pro40_native_interface(){std::abort();}
}
int main() {
    char pattern[]="/tmp/pro40-sustained-entry-XXXXXX";
    const char *dir=mkdtemp(pattern);assert(dir);
    Pro40Packets::InputMonitor monitor;
    Pro40Packets::InputRecording recording;
    Pro40::NativeSessionOptions good;
    good.guid=0x00130e0400000001;good.logPath=std::string(dir)+"/session.log";
    good.retainHistory=false;good.sustained=true;good.stopAfterSeconds=90;good.inputMonitor=&monitor;
    auto handoff=std::make_unique<Pro40Audio::SustainedInputHandoff>(125,3);
#ifdef PRO40_SUSTAINED_INPUT
    good.inputHandoff=handoff.get();
#else
    auto refused=good;refused.inputHandoff=handoff.get();
    assert(Pro40::runNativeSession(refused).exitCode==64);
#endif
    auto playback=std::make_unique<Pro40Audio::PlaybackPcmQueue<>>(1);
    auto port=playback->port();
#ifdef PRO40_LIVE_PLAYBACK
    good.livePlayback=&port;
    auto malformed=good;
    auto invalid=port;invalid.channels=7;malformed.livePlayback=&invalid;
    assert(Pro40::runNativeSession(malformed).exitCode==64);
    malformed=good;malformed.inputHandoff=nullptr;
    assert(Pro40::runNativeSession(malformed).exitCode==64);
    Pro40Packets::PlaybackClip clip;malformed=good;malformed.playback=&clip;
    assert(Pro40::runNativeSession(malformed).exitCode==64);
#else
    auto unsupported=good;unsupported.livePlayback=&port;
    assert(Pro40::runNativeSession(unsupported).exitCode==64);
#endif
    good.stop={[](void *)noexcept{return true;},nullptr};
    for(unsigned error=0;error<9;++error) {
        auto bad=good;
        switch(error) {
        case 0:bad.sustained=false;break;
        case 1:bad.inputMonitor=nullptr;break;
        case 2:bad.stopAfterSeconds=3601;break;
        case 3:bad.recording=&recording;break;
        case 4:bad.retainHistory=true;break;
        case 5:bad.warmupOnly=true;break;
        case 6:bad.startupRecovery=true;break;
        case 7:bad.ownedIdleObservation=true;break;
        case 8:monitor.frames=1;break;
        }
        assert(Pro40::runNativeSession(bad).exitCode==64);monitor.frames=0;
    }
    auto poisoned=std::make_unique<Pro40Audio::SustainedInputHandoff>(0,3);
    auto bad=good;bad.inputHandoff=poisoned.get();
    assert(Pro40::runNativeSession(bad).exitCode==64);
    assert(Pro40::runNativeSession(good).outcome->reason==Pro40::StopReason::interrupted);
    good.stopAfterSeconds=0;
    assert(Pro40::runNativeSession(good).outcome->reason==Pro40::StopReason::interrupted);
    assert(opens==0 && closes==0 && std::filesystem::is_empty(dir));
    good.stop={};
    const auto failed=Pro40::runNativeSession(good);
    assert(failed.exitCode==2 && failed.outcome && failed.outcome->cleanup==0);
    assert(opens==1 && closes==1);
    std::filesystem::remove_all(dir);
    std::puts("PASS: sustained-only admission, finite recorder/history refusal, cancellation before IO and clean native-open failure");
}
