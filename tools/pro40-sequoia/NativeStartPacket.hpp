// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeStartDescriptor.hpp"
#include <array>
#include <algorithm>
#include <span>
namespace Pro40Audio {
// Private versioned IPC value, never a mapped C++ struct or FireWire payload.
// No fit-quality approval, endpoint authentication or permission to start HAL.
struct NativeStartMessage {
    enum class Kind:uint32_t {offer=1,echo=2,commit=3,ready=4};
    Kind kind=Kind::offer;uint64_t nonce=0;NativeStartDescriptor descriptor;
};
class NativeStartPacket {
    static uint64_t get(std::span<const uint8_t> b,size_t at,size_t n)noexcept{
        uint64_t v=0;for(size_t i=0;i<n;++i)v|=uint64_t(b[at+i])<<(8*i);return v;
    }
    static void put(std::span<uint8_t> b,size_t at,size_t n,uint64_t v)noexcept{
        for(size_t i=0;i<n;++i)b[at+i]=uint8_t(v>>(8*i));
    }
public:
    using Message=NativeStartMessage;
    using Descriptor=NativeStartDescriptor;
    static constexpr size_t size=144;
    static bool valid(const NativeStartMessage &m)noexcept{
        return uint32_t(m.kind)>=1&&uint32_t(m.kind)<=4&&m.nonce&&
            m.descriptor.initialFrames==512&&m.descriptor.clock.horizonFrames<=2880000&&
            m.descriptor.validAt(m.descriptor.preparedHost);
    }
    static bool encode(const NativeStartMessage &m,std::span<uint8_t> out)noexcept{
        if(out.size()!=size||!valid(m))return false;
        std::array<uint8_t,size> b{};const auto &d=m.descriptor;const auto &c=d.clock;const auto &id=c.identity;
        put(b,0,4,0x304e3450);put(b,4,2,1);put(b,6,2,size); // P4N0 v1, LE.
        put(b,8,4,uint32_t(m.kind));put(b,16,8,m.nonce);
        put(b,24,8,id.epoch);put(b,32,8,id.seed);put(b,40,4,id.generation);
        put(b,44,4,id.numer);put(b,48,4,id.denom);
        put(b,56,8,c.origin);put(b,64,8,c.ticksNumerator);put(b,72,8,c.ticksDenominator);put(b,80,8,c.horizonFrames);
        put(b,88,8,d.evidenceSequence);put(b,96,8,d.prefixFrames);put(b,104,8,d.evidenceHost);
        put(b,112,8,d.preparedHost);put(b,120,8,d.expiresHost);put(b,128,8,d.initialFrames);
        put(b,136,4,48000);put(b,140,2,1);put(b,142,2,24);
        std::copy(b.begin(),b.end(),out.begin());return true;
    }
    static bool decode(std::span<const uint8_t> b,NativeStartMessage &out)noexcept{
        if(b.size()!=size||get(b,0,4)!=0x304e3450||get(b,4,2)!=1||get(b,6,2)!=size||
           get(b,12,4)||get(b,52,4)||get(b,136,4)!=48000||get(b,140,2)!=1||get(b,142,2)!=24)return false;
        NativeStartMessage m;m.kind=NativeStartMessage::Kind(get(b,8,4));m.nonce=get(b,16,8);
        auto &d=m.descriptor;auto &c=d.clock;
        c.identity={get(b,24,8),get(b,32,8),uint32_t(get(b,40,4)),uint32_t(get(b,44,4)),uint32_t(get(b,48,4))};
        c.origin=get(b,56,8);c.ticksNumerator=get(b,64,8);c.ticksDenominator=get(b,72,8);c.horizonFrames=get(b,80,8);
        d.evidenceSequence=get(b,88,8);d.prefixFrames=get(b,96,8);d.evidenceHost=get(b,104,8);
        d.preparedHost=get(b,112,8);d.expiresHost=get(b,120,8);d.initialFrames=get(b,128,8);
        if(!valid(m))return false;out=m;return true;
    }
};
}
