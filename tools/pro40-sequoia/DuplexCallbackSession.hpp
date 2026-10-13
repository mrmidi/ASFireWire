// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SustainedClockPublication.hpp"
#include "AudioCoreAudioWrite.hpp"
#include "PlaybackPcmQueue.hpp"
namespace Pro40Audio {
// Callback-facing session kernel, not installed HAL properties or a client
// manager. Its owner must admit BOTH mappings, prime output, publish only after
// startup, and quiesce callbacks/native consumer before destruction. The output
// mapping is supplied explicitly; never infer it from the input clock fit.
template<size_t Capacity=4096,size_t Inputs=8,size_t Outputs=6,size_t OutputCapacity=8192>
class DuplexCallbackSession {
public:
    using HostClock=uint64_t(*)(uint64_t) noexcept;
    static uint64_t suppliedHostTime(uint64_t hint) noexcept {return hint;}
private:
    SustainedClockPublication<Capacity,Inputs> &input_;
    PlaybackPcmQueue<OutputCapacity,Outputs> &output_;
    const AudioCallbackTimeline inputTimeline_,outputTimeline_;
    const HostClock hostClock_;
    bool valid_=false;
    bool outputStarted_=false; // Serialized mixed-output callback owner only.
    // Fault-only diagnostic: 0 none, 1 invalid mapping, 2 no pinned publication,
    // 3 unusable/expired clock metadata. No realtime IO or successful-path log.
    std::atomic<uint32_t> outputAdmissionFailure_{0};
    // Input fault detail, also emitted only after the connection owner joins.
    // 1 mapping, 2 output fault, 3 no lease, 4 clock metadata,
    // 10+WindowResult, 20+TimelineResult. No callback logging or allocation.
    std::atomic<uint32_t> inputFailure_{0},inputClockFailure_{0};
    std::atomic<uint64_t> inputOldest_{0},inputNext_{0};
public:
    uint64_t inputOldest()const noexcept{return inputOldest_.load(std::memory_order_relaxed);}
    uint64_t inputNext()const noexcept{return inputNext_.load(std::memory_order_relaxed);}
    uint32_t outputAdmissionFailure() const noexcept {return outputAdmissionFailure_.load(std::memory_order_relaxed);}
    uint32_t inputFailure()const noexcept{return inputFailure_.load(std::memory_order_relaxed);}
    uint32_t inputClockFailure()const noexcept{return inputClockFailure_.load(std::memory_order_relaxed);}
    static constexpr size_t inputChannels=Inputs,outputChannels=Outputs;
    DuplexCallbackSession(SustainedClockPublication<Capacity,Inputs> &input,
        PlaybackPcmQueue<OutputCapacity,Outputs> &output,
        AudioCallbackTimeline inputTimeline,AudioCallbackTimeline outputTimeline,HostClock hostClock=suppliedHostTime)
        :input_(input),output_(output),inputTimeline_(inputTimeline),outputTimeline_(outputTimeline),hostClock_(hostClock) {
        uint64_t ignored=0;
        valid_=hostClock_ && inputTimeline.feedEpoch==outputTimeline.feedEpoch && inputTimeline.clockSeed==outputTimeline.clockSeed &&
            inputTimeline.map(inputTimeline.feedEpoch,inputTimeline.clockSeed,double(inputTimeline.callbackSample),true,1,ignored)==TimelineResult::ok &&
            outputTimeline.map(outputTimeline.feedEpoch,outputTimeline.clockSeed,double(outputTimeline.callbackSample),true,1,ignored)==TimelineResult::ok;
    }
    bool timestamp(uint64_t now,SourceFrameClock::Stamp &out) noexcept {
        if(!valid_ || output_.fault()!=PlaybackQueueResult::ok)return false;
        auto lease=input_.tryAcquire();
        if(!lease)return false;
        // The live clock is sampled AFTER the publication is pinned: a newer
        // publication may legitimately postdate the caller's pre-acquire hint.
        // Deterministic models use suppliedHostTime. No freshness guard changes.
        now=hostClock_(now);
        const auto m=lease->metadata();
        if(!m.usable(inputTimeline_.feedEpoch,inputTimeline_.clockSeed,now))return false;
        out=m.stamp;return true;
    }
    bool readInput(uint64_t now,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<float> out) noexcept {
        auto lease=input_.tryAcquire();
        if(valid_ && lease)now=hostClock_(now);
        if(!valid_ || output_.fault()!=PlaybackQueueResult::ok || !lease ||
           !lease->metadata().usable(inputTimeline_.feedEpoch,inputTimeline_.clockSeed,now)) {
            inputFailure_.store(!valid_?1:output_.fault()!=PlaybackQueueResult::ok?2:!lease?3:4,std::memory_order_relaxed);
            if(lease){const auto m=lease->metadata();inputClockFailure_.store(
                (!m.ready?1:0)|(m.epoch!=inputTimeline_.feedEpoch?2:0)|(m.stamp.seed!=inputTimeline_.clockSeed?4:0)|
                (now<m.publishedHost?8:0)|(now>m.expiresHost?16:0)|(m.stamp.host>now?32:0),std::memory_order_relaxed);}
            std::fill(out.begin(),out.end(),0);return false;
        }
        const auto read=readCoreAudioInput(inputTimeline_,*lease,inputTimeline_.feedEpoch,inputTimeline_.clockSeed,cycle,frames,out);
        if(!read.ok()){
            inputOldest_.store(lease->oldest(),std::memory_order_relaxed);inputNext_.store(lease->next(),std::memory_order_relaxed);
            inputFailure_.store(read.timeline!=TimelineResult::ok?20+uint32_t(read.timeline):10+uint32_t(read.window),std::memory_order_relaxed);
        }
        return read.ok();
    }
    // One serialized mixed-output callback owner (not one producer per client).
    // A rejected callback cannot be retried from an unowned host buffer.
    bool writeOutput(uint64_t now,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<const float> samples) noexcept {
        auto lease=input_.tryAcquire();
        if(valid_ && lease)now=hostClock_(now);
        if(!valid_ || !lease || !lease->metadata().usable(outputTimeline_.feedEpoch,outputTimeline_.clockSeed,now)) {
            outputAdmissionFailure_.store(!valid_?1:!lease?2:3,std::memory_order_relaxed);
            output_.fail(PlaybackQueueResult::state);return false;
        }
        // CoreAudio selects the first interval (actual host timing experiment:
        // DUPLEX_HOST_TIMING.md), rather than starting at our primed queue end.
        // Cover only that initial, unrequested range with bounded silence. The
        // admitted physical mapping stays fixed; every later write is exact.
        const bool ok=writeCoreAudioOutput(outputTimeline_,output_,outputTimeline_.feedEpoch,
            outputTimeline_.clockSeed,cycle,frames,samples,!outputStarted_).ok();
        if(ok)outputStarted_=true;
        return ok;
    }
};
}
