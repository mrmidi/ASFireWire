// SPDX-License-Identifier: Apache-2.0
#pragma once
// Test-only independent packet/timer generator. Does not call the TX encoder.
#include "SustainedInputHandoff.hpp"
#include <cmath>
namespace Pro40Audio::Fixture {
inline int32_t sample(uint64_t frame,unsigned channel) {
    return int32_t((frame*17+channel*54321)%16000001)-8000000;
}
struct Source {
    static constexpr uint64_t firstBus=(127*8000ULL+7990)*3072;
    static constexpr uint64_t hostOrigin=10000000000000000ULL;
    static constexpr uint32_t generation=7;
    uint64_t ordinal=0,frames=0;
    long double nextSyt=firstBus+8192.L;
    long double ticksPerFrame=512;
    PacketClockRecord record{};
    std::array<uint8_t,360> storage{};
    std::span<const uint8_t> next() {
        const auto arrival=firstBus+ordinal*3072;
        const bool data=nextSyt<=arrival+8192;
        const auto sytTicks=uint64_t(std::round(nextSyt));
        const uint32_t syt=data?uint32_t(((sytTicks/3072)%16)<<12 | sytTicks%3072):0xffff;
        record={ordinal,frames,generation,uint32_t((arrival/3072)%64000),syt,data?8u:0u,uint32_t(frames%256)};
        Pro40Packets::write32(storage.data(),3u<<24 | 11u<<16 | record.dbc);
        Pro40Packets::write32(storage.data()+4,0x90020000u|syt);
        if(data)for(unsigned f=0;f<8;++f)for(unsigned c=0;c<11;++c)
            Pro40Packets::write32(storage.data()+8+f*44+c*4,c==10?0x80000000:
                0x40000000u|(uint32_t(sample(frames+f,c))&0xffffff));
        ++ordinal;
        if(data){frames+=8;nextSyt+=8*ticksPerFrame;}
        return std::span(storage).first(data?360:8);
    }
    ControllerClockPair clock(bool anchor=false) const {
        const auto bus=anchor?firstBus-3072:firstBus+ordinal*3072;
        const auto cycles=anchor?0:ordinal+1;
        // Controller clock is 40 ppm faster than the independent host clock.
        const auto host=hostOrigin+uint64_t(std::round(cycles*3000.L/1.00004L));
        const auto seconds=bus/24576000;
        const uint32_t timer=uint32_t((seconds%128)<<25 | ((bus/3072)%8000)<<12 | bus%3072);
        return {host-50,host,host+50,generation,timer,0,uint32_t(ordinal),anchor};
    }
};
}
