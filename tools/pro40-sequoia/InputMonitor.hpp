// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "Pro40Packets.hpp"
#include <array>
#include <cstdint>

namespace Pro40Packets {
// Fixed-size content evidence for sustained transport checks. No recording
// buffer, allocations, callbacks, file writes or retaining native memory.
// PCM slot order: references/linux-dice/amdtp-am824.c:99-103,179-207.
class InputMonitor {
public:
    struct Channel {
        uint64_t nonzero=0,firstNonzero=UINT64_MAX,lastNonzero=UINT64_MAX;
        int32_t minimum=0,maximum=0;
    };
    std::array<Channel,8> channels{};
    uint64_t packets=0,frames=0,prerollFrames=0;
    bool fault=false;
    bool observe(std::span<const uint8_t> payload,bool preroll=false) noexcept {
        if(fault)return false;
        if((payload.size()!=8 && payload.size()!=360) || frames>UINT64_MAX-8 || packets==UINT64_MAX)
            return !(fault=true);
        if(preroll) {
            if(frames || payload.size()!=360)return !(fault=true);
            ++packets;frames=prerollFrames=8;return true;
        }
        for(size_t offset=8;offset<payload.size();offset+=44)
            for(unsigned c=0;c<8;++c)if(payload[offset+c*4]!=0x40)return !(fault=true);
        ++packets;
        for(size_t offset=8;offset<payload.size();offset+=44,++frames)
            for(unsigned c=0;c<8;++c) {
                const auto bits=read32(payload.data()+offset+c*4)&0xffffff;
                const auto value=int32_t(int64_t(bits)-((bits&0x800000)?0x1000000:0));
                auto &channel=channels[c];
                if(frames==prerollFrames || value<channel.minimum)channel.minimum=value;
                if(frames==prerollFrames || value>channel.maximum)channel.maximum=value;
                if(value){if(!channel.nonzero)channel.firstNonzero=frames;channel.lastNonzero=frames;++channel.nonzero;}
            }
        return true;
    }
};
}
