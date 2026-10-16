// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <span>
namespace Pro40Audio {
// Private synthetic start payload, not FireWire and not a native capture clock.
// Source frame zero == HAL sample zero at origin mach_absolute_time ticks.
// Safety offset is a HAL scheduling property, never added to this mapping.
struct AudioSyntheticStart {
    uint64_t nonce=0,epoch=0,seed=0,origin=0;
    uint32_t numer=0,denom=0;
    uint64_t frames=0;
    static constexpr uint64_t maxFrames=48000*60;
    bool operator==(const AudioSyntheticStart &) const =default;
    bool deadline(uint64_t endFrame,uint64_t &ticks) const noexcept {
        if(!numer || !denom || !endFrame || endFrame>frames)return false;
        using Wide=__uint128_t;
        const Wide divisor=Wide(48000)*numer;
        const Wide numerator=Wide(endFrame)*1000000000*denom;
        const Wide offset=(numerator+divisor-1)/divisor; // Never release early.
        if(offset>UINT64_MAX-origin)return false;
        ticks=origin+uint64_t(offset);return true;
    }
    bool valid() const noexcept {
        uint64_t last=0;
        return nonce && epoch && seed && numer && denom && frames && frames<=maxFrames &&
            frames%512==0 && deadline(frames,last);
    }
    bool matchesTimebase(uint32_t localNumer,uint32_t localDenom) const noexcept {
        return valid() && numer==localNumer && denom==localDenom;
    }
};
class AudioSyntheticStartPacket {
    static uint64_t get(std::span<const uint8_t> bytes,size_t at,size_t count) noexcept {
        uint64_t value=0;for(size_t n=0;n<count;++n)value|=uint64_t(bytes[at+n])<<(8*n);return value;
    }
    static void put(std::span<uint8_t> bytes,size_t at,size_t count,uint64_t value) noexcept {
        for(size_t n=0;n<count;++n)bytes[at+n]=uint8_t(value>>(8*n));
    }
public:
    static constexpr size_t size=64;
    static bool encode(const AudioSyntheticStart &s,std::span<uint8_t> bytes) noexcept {
        if(bytes.size()!=size || !s.valid())return false;
        put(bytes,0,4,0x30533450); // P4S0; all fields explicitly little endian.
        put(bytes,4,2,1);put(bytes,6,2,size);
        put(bytes,8,8,s.nonce);put(bytes,16,8,s.epoch);put(bytes,24,8,s.seed);
        put(bytes,32,8,s.origin);put(bytes,40,4,s.numer);put(bytes,44,4,s.denom);
        put(bytes,48,4,48000);put(bytes,52,4,512);put(bytes,56,8,s.frames);return true;
    }
    static bool decode(std::span<const uint8_t> bytes,AudioSyntheticStart &out) noexcept {
        if(bytes.size()!=size || get(bytes,0,4)!=0x30533450 || get(bytes,4,2)!=1 ||
            get(bytes,6,2)!=size || get(bytes,48,4)!=48000 || get(bytes,52,4)!=512)return false;
        AudioSyntheticStart s{get(bytes,8,8),get(bytes,16,8),get(bytes,24,8),get(bytes,32,8),
            uint32_t(get(bytes,40,4)),uint32_t(get(bytes,44,4)),get(bytes,56,8)};
        if(!s.valid())return false;out=s;return true;
    }
};
}
