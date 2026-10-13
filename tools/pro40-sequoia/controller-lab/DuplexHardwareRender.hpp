// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../SourceFrameClock.hpp"
#include <array>
#include <algorithm>
#include <CoreAudio/AudioServerPlugIn.h>
#include <cmath>
#include <span>
namespace Pro40Lab {
// Bounded diagnostic content, no microphone monitoring or routing writes.
// Same -30 dBFS, 440 Hz, 5 ms fades and 18-second sequence as LivePlaybackCheck.
struct DuplexSpeakerContent {
    static constexpr unsigned seconds=90;
    static void tones(uint64_t first,std::span<float> out) noexcept {
        std::fill(out.begin(),out.end(),0.f);
        constexpr double tau=6.283185307179586476925286766559;
        for(size_t f=0;f<out.size()/6;++f) {
            const auto phase=(first+f)%(18*48000);
            if(phase<2*48000 || phase>=14*48000)continue;
            const auto offset=phase-2*48000,channel=offset/(2*48000),sample=offset%(2*48000);
            if(sample>=48000)continue;
            const double fade=sample<240?double(sample)/240:sample>=48000-240?double(48000-1-sample)/240:1.;
            out[f*6+channel]=float((265271./8388608.)*fade*std::sin(tau*440*sample/48000));
        }
    }
    void observe(uint64_t,std::span<const float>) noexcept {}
};
template<class Content=DuplexSpeakerContent> struct BasicDuplexHardwareRender {
    static constexpr unsigned frames=137,blocks=(Content::seconds*48000+frames-1)/frames;
    Content content;
    std::array<double,8> peak{};
    std::array<uint64_t,8> nonzero{};
    uint64_t began=0,ended=0,maxLateTicks=0,firstInput=0,firstOutput=0,firstOutputSample=0;
    unsigned completed=0,failure=0;
    // Rational pacing from the admitted hardware clock; never assume the Mac
    // and device are exactly 48 kHz. This bounded diagnostic keeps the admitted
    // slope fixed; it does not establish production clock renewal/latency policy.
    static bool due(const Pro40Audio::SourceFrameClock &clock,uint64_t began,uint64_t frame,uint64_t &out) noexcept {
        if(!clock.valid())return false;
        const __uint128_t delta=(__uint128_t(frame)*clock.ticksNumerator+clock.ticksDenominator-1)/clock.ticksDenominator;
        if(delta>UINT64_MAX-began)return false;
        out=began+uint64_t(delta);return true;
    }
    template<class Host,class Cancel,class Now,class Wait>
    bool run(Host &host,const Cancel &cancel,Now now,Wait wait) {
        AudioServerPlugInIOCycleInfo cycle{};
        std::array<float,frames*8> input{};std::array<float,frames*6> output{};
        firstInput=host.descriptor.inputFirst;firstOutput=host.descriptor.outputFirst+host.descriptor.primedOutputFrames;
        began=now();
        const auto fail=[&](unsigned why){failure=why;ended=now();return false;};
        const __int128 outputSample=__int128(firstOutput)+host.descriptor.outputTimeline.callbackSample-host.descriptor.outputTimeline.feedFrame;
        if(outputSample<0 || outputSample>Pro40Audio::SourceFrameClock::maxExactFrame-uint64_t(blocks)*frames)return fail(5);
        firstOutputSample=uint64_t(outputSample);
        for(unsigned block=0;block<blocks;++block) {
            uint64_t target=0;
            if(!due(host.descriptor.clock,began,uint64_t(block)*frames,target))return fail(5);
            if(!wait(target))return fail(6);
            const auto current=now();
            if(current<target)return fail(6);
            maxLateTicks=std::max(maxLateTicks,current-target);
            if(cancel())return fail(1);
            cycle.mInputTime.mSampleTime=double(firstInput+uint64_t(block)*frames);
            cycle.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
            cycle.mNominalIOBufferFrameSize=frames;
            if(!host.readInput(current,cycle,frames,input))return fail(2);
            for(unsigned f=0;f<frames;++f)for(unsigned c=0;c<8;++c) {
                const auto sample=input[f*8+c];if(!std::isfinite(sample)||std::abs(sample)>1.f)return fail(3);
                peak[c]=std::max(peak[c],double(std::abs(sample)));nonzero[c]+=sample!=0.f;
            }
            content.observe(uint64_t(block)*frames,input);
            content.tones(uint64_t(block)*frames,output);
            cycle.mOutputTime.mSampleTime=double(firstOutputSample+uint64_t(block)*frames);
            cycle.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;
            if(!host.writeOutput(now(),cycle,frames,output))return fail(4);
            ++completed;
        }
        ended=now();Pro40Audio::SourceFrameClock::Stamp stamp;
        return host.timestamp(ended,stamp) || fail(5);
    }
};
using DuplexHardwareRender=BasicDuplexHardwareRender<>;
static_assert(DuplexHardwareRender::blocks==31533);
}
