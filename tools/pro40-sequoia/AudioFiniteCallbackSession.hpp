// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFiniteWorkerSession.hpp"
#include "AudioCoreAudioRead.hpp"
namespace Pro40Audio {
struct FiniteCallbackResult {
    TimelineResult timeline=TimelineResult::invalidRange;
    WindowResult window=WindowResult::invalidRange;
    size_t audioFrames=0,paddingFrames=0;
    uint64_t epoch=0,seed=0,completionThrough=0;
    bool ok() const {return timeline==TimelineResult::ok && window==WindowResult::ok;}
};
// Local bounded variable-size callback lab. Immutable clock/epoch; one logical reader
// completion. Not an installed HAL session or a native startup clock estimator.
template<size_t Capacity> class AudioFiniteCallbackSession:public AudioFiniteWorkerSession<Capacity> {
    using Base=AudioFiniteWorkerSession<Capacity>;
    const AudioCallbackTimeline timeline_;
    using Base::postDrainAck; // Callback composition uses clock-checked completeRead.
public:
    AudioFiniteCallbackSession(AudioCallbackTimeline timeline,uint64_t first):Base(timeline.feedEpoch,first),timeline_(timeline) {
        uint64_t mapped=0;
        if(first || timeline.feedFrame || timeline.map(timeline.feedEpoch,timeline.clockSeed,
            double(timeline.callbackSample),true,512,mapped)!=TimelineResult::ok)Base::close();
    }
    FiniteCallbackResult readInput(uint64_t epoch,uint64_t seed,const AudioServerPlugInIOCycleInfo &cycle,
                                  UInt32 actualFrames,std::span<float> out) noexcept {
        std::fill(out.begin(),out.end(),0.0f);
        FiniteCallbackResult result;
        if(out.size()!=actualFrames)return result;
        uint64_t first=0;
        result.timeline=timeline_.map(epoch,seed,cycle.mInputTime.mSampleTime,
            (cycle.mInputTime.mFlags&kAudioTimeStampSampleTimeValid)!=0,actualFrames,first);
        if(result.timeline!=TimelineResult::ok)return result;
        std::array<int32_t,AudioCallbackTimeline::maxFrames> pcm;
        const auto read=Base::readInput(epoch,first,std::span(pcm).first(actualFrames));result.window=read.result;
        if(!read.ok())return result;
        for(size_t n=0;n<out.size();++n)out[n]=float(pcm[n])/8388608.0f;
        result.audioFrames=read.audioFrames;result.paddingFrames=read.paddingFrames;
        result.epoch=epoch;result.seed=seed;
        // ACK uses the protocol's rounded final-frame token, independent of
        // callback geometry. audioFrames excludes padding; finish() bounds END
        // so rounding cannot overflow. This is not a count of frames consumed.
        if(read.reachedEnd)result.completionThrough=((first+read.audioFrames+511)/512)*512;
        return result;
    }
    // Call AFTER readInput returns and its returned samples have been handled.
    // Bounded mailbox only, no retry. Trusted caller, not proof of consumption.
    bool completeRead(const FiniteCallbackResult &r) noexcept {
        if(!r.ok() || !r.completionThrough || r.epoch!=timeline_.feedEpoch || r.seed!=timeline_.clockSeed)return false;
        std::array<uint8_t,AudioFeedControl::size> message{};
        return AudioFeedControl{AudioFeedControl::Kind::ack,r.epoch,r.completionThrough}.encode(message) && Base::postDrainAck(message);
    }
};
}
