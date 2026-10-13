// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <span>

namespace Pro40Packets {
// Observation only, after stream-0 framing validation. No readiness decisions,
// allocation or IO. Slots match Pro40Packets.hpp and Linux amdtp-am824.c:179-207.
// Keep exact first activity for all ten PCM slots, including unused digital
// slots which may legitimately remain zero. Zero is never considered a fault.
struct InputStartupActivity {
    static constexpr uint64_t absent=std::numeric_limits<uint64_t>::max();
    struct Channel {
        uint64_t firstValidFrame=absent,firstNonzeroFrame=absent,firstNonzeroPacket=absent;
        uint64_t invalidLabels=0;
        uint32_t firstWord=0;
        unsigned block=0;
        std::array<uint8_t,360> packet{};
    };
    std::array<Channel,10> channels{};
    uint64_t packets=0,frames=0;
    bool fault=false;
    void observe(std::span<const uint8_t> payload) noexcept {
        if(fault)return;
        if(packets==262144 || (payload.size()!=8 && payload.size()!=360)) {fault=true;return;}
        const auto ordinal=packets++;
        if(payload.size()==8)return;
        for(unsigned block=0;block<8;++block) {
            for(unsigned slot=0;slot<channels.size();++slot) {
                auto &channel=channels[slot];
                const size_t pos=8+block*44+slot*4;
                if(payload[pos]!=0x40) {++channel.invalidLabels;continue;}
                if(channel.firstValidFrame==absent)channel.firstValidFrame=frames+block;
                if(channel.firstNonzeroFrame!=absent)continue;
                const uint32_t word=uint32_t(payload[pos])<<24|uint32_t(payload[pos+1])<<16|
                    uint32_t(payload[pos+2])<<8|payload[pos+3];
                if(!(word&0xffffff))continue;
                channel.firstNonzeroFrame=frames+block;channel.firstNonzeroPacket=ordinal;
                channel.firstWord=word;channel.block=block;
                for(size_t n=0;n<payload.size();++n)channel.packet[n]=payload[n];
            }
        }
        frames+=8;
    }
};
}
