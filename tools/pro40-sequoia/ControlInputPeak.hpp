// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
namespace Pro40 {
// Original Pro40 input1 only. Read-only, but peak reads may consume indicators.
// ALSA tcat/extension.rs:95 (peak section), caps_section.rs:196,288 (capability),
// peak_section.rs:23-40 and router_entry.rs:169-170,202 (raw upper16-bit peak).
struct ControlInputPeak {
    int32_t ioStatus=0;
    unsigned error=0; // 1 layout, 2 unavailable, 3 unexpected source/destination
    uint32_t raw=0;
    uint16_t peak=0;
    bool valid=false;
    template<class Read,class Guard> void capture(Read read,Guard guard){
        *this={};
        constexpr uint64_t base=0xffffe0200000ULL,window=0x200000;
        std::array<uint8_t,8> bytes{};
        auto be=[](const uint8_t *p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];};
        auto get=[&](uint64_t a,unsigned n){ioStatus=guard();if(!ioStatus)ioStatus=read(a,n,bytes.data());return !ioStatus;};
        if(!get(base,8))return;
        const uint64_t caps=uint64_t(be(bytes.data()))*4,capSize=uint64_t(be(bytes.data()+4))*4;
        if(!get(base+24,8))return;
        const uint64_t peaks=uint64_t(be(bytes.data()))*4,peakSize=uint64_t(be(bytes.data()+4))*4;
        if(caps<72 || caps>window || capSize<12 || capSize>window-caps ||
           peaks<72 || peaks>window || peakSize<4 || peakSize>window-peaks ||
           (caps<peaks+peakSize && peaks<caps+capSize)){error=1;return;}
        if(!get(base+caps+8,4))return;
        if(!(be(bytes.data())&4)){error=2;return;}
        if(!get(base+peaks,4))return;
        raw=be(bytes.data());peak=uint16_t(raw>>16);
        // Captured original Pro40 slot0: Ins1:0 -> Avs0:0. Do not label other
        // routing as microphone silence. Do not normalize the raw peak field.
        if((raw&0xffff)!=0x50b0){error=3;return;}
        ioStatus=guard();valid=!ioStatus;
    }
};
}
