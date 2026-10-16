// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
namespace Pro40 {
// Read-only low-rate current router view. Behavioral references:
// ALSA protocols/dice/src/tcat/extension.rs:98 and
// tcat/extension/current_config_section.rs:29-86; focusrite/spro40.rs:153.
struct MeterRouteSnapshot {
    int32_t ioStatus=0;
    unsigned error=0; // 1 section bounds, 2 route count, 3 ENABLE changed
    uint32_t count=0,enableBefore=0,enableAfter=0;
    std::array<uint32_t,8> words{};
    bool valid=false;
    template<class Read,class Stable> void capture(uint64_t global,Read read,Stable stable) {
        *this={};
        constexpr uint64_t base=0xffffe0200000ULL,window=0x200000;
        std::array<uint8_t,36> raw{};
        auto be=[](const uint8_t *p) {return uint32_t(p[0])<<24|uint32_t(p[1])<<16|
            uint32_t(p[2])<<8|uint32_t(p[3]);};
        auto get=[&](uint64_t address,unsigned size) {
            ioStatus=read(address,size,raw.data());return ioStatus==0;
        };
        if(!get(base+48,8))return;
        const uint64_t offset=uint64_t(be(raw.data()))*4,size=uint64_t(be(raw.data()+4))*4;
        if(offset<72 || offset>window || size<36 || size>window-offset) {error=1;return;}
        if(!get(global+0x50,4))return;
        enableBefore=be(raw.data());
        if(!get(base+offset,36))return;
        count=be(raw.data());
        if(count<8 || count>1023 || 4+uint64_t(count)*4>size) {error=2;return;}
        for(unsigned i=0;i<8;++i)words[i]=be(raw.data()+4+4*i);
        if(!get(global+0x50,4))return;
        enableAfter=be(raw.data());
        if(enableBefore!=enableAfter) {error=3;return;}
        ioStatus=stable();valid=ioStatus==0;
    }
};
}
