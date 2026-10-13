// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DiceBlockGeometry.hpp"
#include <array>
#include <cstdint>

// Idle-only, reversible DICE channel configuration. Linux dice-stream.c:326-369
// and dice-interface.h:244-262,306-325 define channel/speed and asymmetric fields.
// The backend guard must verify captured generation, our ownership, ENABLE=0,
// and stable internal 48 kHz. No ENABLE or routing writes exist in this class.
template<class Backend> class Pro40ChannelConfig {
    struct Register { uint64_t address=0; uint32_t before=0, desired=0; bool touched=false; };
    Backend &io_;
    std::array<Register,6> registers_{};
    bool attempted_=false, restored_=false;
public:
    static constexpr int32_t invalid=-1, mismatch=-2;
    explicit Pro40ChannelConfig(Backend &io):io_(io) {}
    Pro40ChannelConfig(const Pro40ChannelConfig &)=delete;
    Pro40ChannelConfig &operator=(const Pro40ChannelConfig &)=delete;
    ~Pro40ChannelConfig() { (void)restore(); }
    int32_t configure(const std::array<uint8_t,4> &channels) {
        if (attempted_ || restored_) return invalid;
        attempted_=true;
        for (unsigned i=0;i<4;++i) {
            if (channels[i]>=63) return invalid;
            for (unsigned j=0;j<i;++j) if (channels[i]==channels[j]) return invalid;
        }
        auto status=io_.guard(); if (status) return status;
        constexpr uint64_t base=0xffffe0000000ULL;
        std::array<uint64_t,3> offsets{},sizes{};
        for (unsigned i=0;i<3;++i) {
            uint32_t offset=0,size=0;
            status=io_.read(base+i*8,offset); if (status) return status;
            status=io_.read(base+i*8+4,size); if (status) return status;
            offsets[i]=uint64_t(offset)*4; sizes[i]=uint64_t(size)*4;
            if (!Pro40::validDiceBlock(offsets[i],sizes[i],8)) return invalid;
            for(unsigned j=0;j<i;++j)
                if (Pro40::diceBlocksOverlap(offsets[i],sizes[i],offsets[j],sizes[j])) return invalid;
        }
        for (unsigned direction=0;direction<2;++direction) {
            const auto address=base+offsets[direction+1];
            uint32_t count=0,stride=0;
            status=io_.read(address,count); if(status) return status;
            status=io_.read(address+4,stride); if(status) return status;
            const uint64_t bytes=uint64_t(stride)*4;
            if(count!=2 || bytes<16 || 8+count*bytes>sizes[direction+1]) return invalid;
            for(unsigned stream=0;stream<2;++stream) {
                const auto streamAddress=address+8+stream*bytes;
                uint32_t pcm=0,midi=0;
                const auto fields=direction?8u:4u; // RX inserts SEQ_START.
                status=io_.read(streamAddress+fields,pcm); if(status) return status;
                status=io_.read(streamAddress+fields+4,midi); if(status) return status;
                const unsigned expectedPcm=direction?(stream?8:12):10;
                if(pcm!=expectedPcm || midi!=(stream?0u:1u)) return invalid;
                if(direction==0) {
                    registers_[stream*2]={streamAddress,0,channels[stream],false};
                    registers_[stream*2+1]={streamAddress+12,0,2,false}; // S400
                } else registers_[4+stream]={streamAddress,0,channels[2+stream],false};
            }
        }
        // Read every original value before the first mutation.
        for(auto &r:registers_) { status=io_.read(r.address,r.before); if(status) return status; }
        for(auto &r:registers_) {
            if(r.before==r.desired) continue;
            status=io_.guard(); if(status) return status;
            r.touched=true; // A failed acknowledgement can still mean a write applied.
            status=io_.write(r.address,r.desired); if(status) return status;
            uint32_t value=0;
            status=io_.read(r.address,value); if(status) return status;
            if(value!=r.desired) return mismatch;
        }
        return io_.guard();
    }
    int32_t restore() {
        if(restored_) return 0;
        restored_=true;
        int32_t result=0;
        for(auto it=registers_.rbegin();it!=registers_.rend();++it) {
            if(!it->touched) continue;
            auto status=io_.guard();
            if(!status) status=io_.write(it->address,it->before);
            uint32_t value=0;
            if(!status) status=io_.read(it->address,value);
            if(!status && value!=it->before) status=mismatch;
            if(status && !result) result=status;
        }
        return result;
    }
};
