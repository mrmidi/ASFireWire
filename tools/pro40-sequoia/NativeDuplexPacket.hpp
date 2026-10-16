// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SourceFrameClock.hpp"
#include "AudioCallbackTimeline.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <span>
namespace Pro40Audio {
// Private, same-host IPC. These are values, never shared C++ objects or a
// FireWire wire format. Identity/nonce must come from the trusted supervisor;
// decoding these bytes is NOT peer authentication or fit-quality admission.
struct NativeDuplexDescriptor {
    SourceFrameClock clock;
    uint64_t evidenceSequence=0,prefixFrames=0,evidenceHost=0,preparedHost=0,expiresHost=0;
    uint32_t sampleRate=48000;
    uint16_t inputChannels=8,outputChannels=6,pcmBits=24;
    uint64_t inputFirst=0,initialInputFrames=0,outputFirst=0,primedOutputFrames=0;
    AudioCallbackTimeline inputTimeline,outputTimeline;
    bool validAt(uint64_t now) const noexcept {
        const auto &id=clock.identity;
        uint64_t last=0,horizon=0,ignored=0;
        const auto mapping=[&](const AudioCallbackTimeline &t) {
            return t.feedEpoch==id.epoch && t.clockSeed==id.seed && t.feedFrame<=SourceFrameClock::maxExactFrame &&
                t.map(id.epoch,id.seed,double(t.callbackSample),true,1,ignored)==TimelineResult::ok;
        };
        const auto range=[&](const AudioCallbackTimeline &t,uint64_t first,size_t count) {
            const __int128 sample=__int128(t.callbackSample)+first-t.feedFrame;
            return sample>=-AudioCallbackTimeline::maxExactSample && sample<=AudioCallbackTimeline::maxExactSample &&
                t.map(id.epoch,id.seed,double(sample),true,count,ignored)==TimelineResult::ok && ignored==first;
        };
        return clock.valid() && sampleRate==48000 && pcmBits==24 &&
            inputChannels>=1 && inputChannels<=20 && outputChannels>=1 && outputChannels<=12 &&
            evidenceSequence && prefixFrames && prefixFrames%8==0 && prefixFrames<=clock.horizonFrames &&
            initialInputFrames && initialInputFrames<=4096 && initialInputFrames%8==0 &&
            inputFirst<=prefixFrames && initialInputFrames<=prefixFrames-inputFirst &&
            primedOutputFrames && primedOutputFrames<=8192 && primedOutputFrames%8==0 &&
            outputFirst<=SourceFrameClock::maxExactFrame-primedOutputFrames &&
            mapping(inputTimeline) && mapping(outputTimeline) &&
            range(inputTimeline,inputFirst,initialInputFrames) && range(outputTimeline,outputFirst+primedOutputFrames,1) &&
            evidenceHost>=clock.origin && preparedHost>=evidenceHost && expiresHost>=preparedHost &&
            now>=preparedHost && now<=expiresHost &&
            clock.hostTime(id,prefixFrames-1,last) && last<=evidenceHost &&
            clock.hostTime(id,clock.horizonFrames,horizon) && expiresHost<=horizon;
    }
    bool operator==(const NativeDuplexDescriptor &b) const noexcept {
        const auto sameMap=[](const auto &x,const auto &y) {
            return x.feedEpoch==y.feedEpoch && x.clockSeed==y.clockSeed &&
                x.feedFrame==y.feedFrame && x.callbackSample==y.callbackSample;
        };
        return clock.identity==b.clock.identity && clock.origin==b.clock.origin &&
            clock.ticksNumerator==b.clock.ticksNumerator && clock.ticksDenominator==b.clock.ticksDenominator &&
            clock.horizonFrames==b.clock.horizonFrames && evidenceSequence==b.evidenceSequence &&
            prefixFrames==b.prefixFrames && evidenceHost==b.evidenceHost && preparedHost==b.preparedHost &&
            expiresHost==b.expiresHost && sampleRate==b.sampleRate && inputChannels==b.inputChannels &&
            outputChannels==b.outputChannels && pcmBits==b.pcmBits && inputFirst==b.inputFirst &&
            initialInputFrames==b.initialInputFrames && outputFirst==b.outputFirst &&
            primedOutputFrames==b.primedOutputFrames && sameMap(inputTimeline,b.inputTimeline) && sameMap(outputTimeline,b.outputTimeline);
    }
};
namespace DuplexWire {
inline uint64_t get(std::span<const uint8_t> b,size_t at,size_t n) noexcept {
    uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(b[at+i])<<(8*i);return v;
}
inline void put(std::span<uint8_t> b,size_t at,size_t n,uint64_t v) noexcept {
    for(size_t i=0;i<n;++i)b[at+i]=uint8_t(v>>(8*i));
}
inline void clock(std::span<uint8_t> b,const SourceFrameClock &c) noexcept {
    const auto &id=c.identity;
    put(b,24,8,id.epoch);put(b,32,8,id.seed);put(b,40,4,id.generation);
    put(b,44,4,id.numer);put(b,48,4,id.denom);
    put(b,56,8,c.origin);put(b,64,8,c.ticksNumerator);put(b,72,8,c.ticksDenominator);put(b,80,8,c.horizonFrames);
}
inline SourceFrameClock clock(std::span<const uint8_t> b) noexcept {
    return {{get(b,24,8),get(b,32,8),uint32_t(get(b,40,4)),uint32_t(get(b,44,4)),uint32_t(get(b,48,4))},
        get(b,56,8),get(b,64,8),get(b,72,8),get(b,80,8)};
}
}
struct NativeDuplexMessage {
    enum class Kind:uint32_t {offer=1,echo=2,commit=3,ready=4};
    Kind kind=Kind::offer;uint64_t nonce=0;NativeDuplexDescriptor descriptor;
};
class NativeDuplexPacket {
public:
    using Message=NativeDuplexMessage;
    using Descriptor=NativeDuplexDescriptor;
    static constexpr size_t size=224;
    static bool valid(const Message &m) noexcept {
        return uint32_t(m.kind)>=1 && uint32_t(m.kind)<=4 && m.nonce && m.descriptor.validAt(m.descriptor.preparedHost);
    }
    static bool encode(const Message &m,std::span<uint8_t> out) noexcept {
        if(out.size()!=size || !valid(m))return false;
        using namespace DuplexWire;
        std::array<uint8_t,size> b{};const auto &d=m.descriptor;
        put(b,0,4,0x30443450);put(b,4,2,1);put(b,6,2,size); // P4D0 v1, LE.
        put(b,8,4,uint32_t(m.kind));put(b,16,8,m.nonce);clock(b,d.clock);
        put(b,88,8,d.evidenceSequence);put(b,96,8,d.prefixFrames);put(b,104,8,d.evidenceHost);
        put(b,112,8,d.preparedHost);put(b,120,8,d.expiresHost);
        put(b,128,4,d.sampleRate);put(b,132,2,d.inputChannels);put(b,134,2,d.outputChannels);put(b,136,2,d.pcmBits);
        put(b,144,8,d.inputFirst);put(b,152,8,d.initialInputFrames);
        put(b,160,8,d.inputTimeline.feedFrame);put(b,168,8,std::bit_cast<uint64_t>(d.inputTimeline.callbackSample));
        put(b,176,8,d.outputFirst);put(b,184,8,d.primedOutputFrames);
        put(b,192,8,d.outputTimeline.feedFrame);put(b,200,8,std::bit_cast<uint64_t>(d.outputTimeline.callbackSample));
        std::copy(b.begin(),b.end(),out.begin());return true;
    }
    static bool decode(std::span<const uint8_t> b,Message &out) noexcept {
        using namespace DuplexWire;
        if(b.size()!=size || get(b,0,4)!=0x30443450 || get(b,4,2)!=1 || get(b,6,2)!=size ||
            get(b,12,4) || get(b,52,4) || get(b,138,6) || get(b,208,8) || get(b,216,8))return false;
        Message m;m.kind=Message::Kind(get(b,8,4));m.nonce=get(b,16,8);auto &d=m.descriptor;
        d.clock=clock(b);const auto &id=d.clock.identity;
        d.evidenceSequence=get(b,88,8);d.prefixFrames=get(b,96,8);d.evidenceHost=get(b,104,8);
        d.preparedHost=get(b,112,8);d.expiresHost=get(b,120,8);
        d.sampleRate=uint32_t(get(b,128,4));d.inputChannels=uint16_t(get(b,132,2));
        d.outputChannels=uint16_t(get(b,134,2));d.pcmBits=uint16_t(get(b,136,2));
        d.inputFirst=get(b,144,8);d.initialInputFrames=get(b,152,8);
        d.inputTimeline={id.epoch,id.seed,get(b,160,8),std::bit_cast<int64_t>(get(b,168,8))};
        d.outputFirst=get(b,176,8);d.primedOutputFrames=get(b,184,8);
        d.outputTimeline={id.epoch,id.seed,get(b,192,8),std::bit_cast<int64_t>(get(b,200,8))};
        if(!valid(m))return false;out=m;return true;
    }
};
struct NativeDuplexClockMessage {
    uint64_t nonce=0;SourceFrameClock clock;
    uint64_t sequence=0,frames=0,observedHost=0;
};
class NativeDuplexClockPacket {
public:
    using Message=NativeDuplexClockMessage;
    static constexpr size_t size=128;
    static bool valid(const Message &m) noexcept {
        uint64_t last=0;
        return m.nonce && m.sequence && m.frames && m.frames%8==0 && m.clock.valid() &&
            m.clock.hostTime(m.clock.identity,m.frames-1,last) && last<=m.observedHost;
    }
    static bool encode(const Message &m,std::span<uint8_t> out) noexcept {
        if(out.size()!=size || !valid(m))return false;
        using namespace DuplexWire;std::array<uint8_t,size> b{};
        put(b,0,4,0x30433450);put(b,4,2,1);put(b,6,2,size); // P4C0 v1, LE.
        put(b,16,8,m.nonce);clock(b,m.clock);put(b,88,8,m.sequence);put(b,96,8,m.frames);put(b,104,8,m.observedHost);
        std::copy(b.begin(),b.end(),out.begin());return true;
    }
    static bool decode(std::span<const uint8_t> b,Message &out) noexcept {
        using namespace DuplexWire;
        if(b.size()!=size || get(b,0,4)!=0x30433450 || get(b,4,2)!=1 || get(b,6,2)!=size ||
            get(b,8,8) || get(b,52,4) || get(b,112,8) || get(b,120,8))return false;
        Message m{get(b,16,8),clock(b),get(b,88,8),get(b,96,8),get(b,104,8)};
        if(!valid(m))return false;out=m;return true;
    }
};
}
