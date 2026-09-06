#include "Audio/Shared/AudioRuntimeTuning.hpp"
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <limits>
using ASFW::Audio::Shared::AudioRuntimeTuning;
struct State {
    uint32_t pendingTxDispatchSlackPackets{0};
    uint32_t pendingTxOwnershipGuardPackets{0};
    uint32_t pendingOutputLatencyFrames{0};
    uint32_t pendingInputLatencyFrames{0};
    uint32_t pendingOutputSafetyOffsetFrames{0};
    uint32_t pendingInputSafetyOffsetFrames{0};
    uint32_t pendingFrameRingFrames{0};
    uint32_t pendingClientIoBudgetFrames{0};
    uint32_t pendingZeroTimestampPeriodFrames{0};
    std::atomic<uint32_t> pendingTuningGroups{0};

    uint32_t activeTxDispatchSlackPackets{0};
    uint32_t activeTxOwnershipGuardPackets{0};
    uint32_t activeOutputLatencyFrames{0};
    uint32_t activeInputLatencyFrames{0};
    uint32_t activeOutputSafetyOffsetFrames{0};
    uint32_t activeInputSafetyOffsetFrames{0};
    uint32_t activeFrameRingFrames{0};
    uint32_t activeClientIoBudgetFrames{0};
    uint32_t activeZeroTimestampPeriodFrames{0};
    std::atomic<uint32_t> activeTuningSequence{0};
    std::atomic<uint32_t> tuningWindowSequence{0};
    std::atomic<bool> audioIoRunning{false};
    uint32_t lastTuningRejection{0};
    uint32_t lastTuningWarnings{0};
};
struct ASFWAudioNub {
 State storage;
 State* ivars=&storage;
 void SetActiveRuntimeTuning(const AudioRuntimeTuning&);
 void CopyActiveRuntimeTuning(AudioRuntimeTuning&) const;
};
void ASFWAudioNub::SetActiveRuntimeTuning(
    const ASFW::Audio::Shared::AudioRuntimeTuning& active)
{
    if (!ivars) return;
    ivars->activeTxDispatchSlackPackets = active.txDispatchSlackPackets;
    ivars->activeTxOwnershipGuardPackets = active.txOwnershipGuardPackets;
    ivars->activeOutputLatencyFrames = active.outputLatencyFrames;
    ivars->activeInputLatencyFrames = active.inputLatencyFrames;
    ivars->activeOutputSafetyOffsetFrames = active.outputSafetyOffsetFrames;
    ivars->activeInputSafetyOffsetFrames = active.inputSafetyOffsetFrames;
    ivars->activeFrameRingFrames = active.frameRingFrames;
    ivars->activeClientIoBudgetFrames = active.clientIoBudgetFrames;
    ivars->activeZeroTimestampPeriodFrames = active.zeroTimestampPeriodFrames;
    ivars->activeTuningSequence.fetch_add(1, std::memory_order_release);
}

void ASFWAudioNub::CopyActiveRuntimeTuning(
    ASFW::Audio::Shared::AudioRuntimeTuning& out) const
{
    if (!ivars) return;
    // Never published yet: leave the caller's defaults, which are the shipping
    // constants, rather than reporting a zeroed geometry the driver never ran.
    if (ivars->activeTuningSequence.load(std::memory_order_acquire) == 0) return;
    out.txDispatchSlackPackets = ivars->activeTxDispatchSlackPackets;
    out.txOwnershipGuardPackets = ivars->activeTxOwnershipGuardPackets;
    out.outputLatencyFrames = ivars->activeOutputLatencyFrames;
    out.inputLatencyFrames = ivars->activeInputLatencyFrames;
    out.outputSafetyOffsetFrames = ivars->activeOutputSafetyOffsetFrames;
    out.inputSafetyOffsetFrames = ivars->activeInputSafetyOffsetFrames;
    out.frameRingFrames = ivars->activeFrameRingFrames;
    out.clientIoBudgetFrames = ivars->activeClientIoBudgetFrames;
    out.zeroTimestampPeriodFrames = ivars->activeZeroTimestampPeriodFrames;
}


int main() {
 using namespace ASFW::Audio::Shared;
 AudioRuntimeTuning overflow;
 overflow.txDispatchSlackPackets=std::numeric_limits<uint32_t>::max();
 const auto outcome=ValidateTuning(overflow);
 std::printf("overflow: applicable=%d rejection=%u target=%u required=%u slots=%u\n",
   outcome.Applicable(), (unsigned)outcome.rejection, overflow.PreparedTargetPackets(),
   overflow.PreparedTargetPackets()+overflow.txOwnershipGuardPackets,
   AudioTimingGeometry::kTxSharedSlotPackets);
 for(unsigned rate: {48000u,96000u,192000u}) {
   auto frames=PreparedLeadFrames(AudioRuntimeTuning{});
   std::printf("rate=%u: reported=%u frames -> %.3f ms; 120 packets -> %u us\n",
    rate,frames,1000.0*frames/rate,PacketsToMicroseconds(120));
 }
 ASFWAudioNub nub;
 AudioRuntimeTuning a,b;
 a.txDispatchSlackPackets=12; a.outputLatencyFrames=12;
 b.txDispatchSlackPackets=48; b.outputLatencyFrames=48;
 nub.SetActiveRuntimeTuning(a);
 std::atomic<bool> stop=false;
 std::thread writer([&]{while(!stop.load(std::memory_order_relaxed)) {
   nub.SetActiveRuntimeTuning(a); nub.SetActiveRuntimeTuning(b);
 }});
 unsigned mismatches=0; unsigned long long reads=0;
 const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(1);
 while(std::chrono::steady_clock::now()<end) {
   AudioRuntimeTuning read;
   nub.CopyActiveRuntimeTuning(read); ++reads;
   if(read.txDispatchSlackPackets!=read.outputLatencyFrames) {
     if(mismatches++==0) std::printf("torn active: slack=%u latency=%u (writer always pairs equal values)\n",
       read.txDispatchSlackPackets,read.outputLatencyFrames);
   }
 }
 stop.store(true); writer.join();
 std::printf("active snapshot: %u mismatches across %llu reads\n",mismatches,reads);
}
