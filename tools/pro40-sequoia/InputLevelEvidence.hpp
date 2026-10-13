// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <span>
namespace Pro40Packets {
// Content-only observation after existing capture/sequence validation. Stream0
// at 48kHz: CIP8 + frames of eleven AM824 quadlets, input1 first. Matches
// Pro40Packets.hpp capture geometry and verify_mic_lock.py's retained captures.
// Fixed buckets; no allocation, IO, routing writes or acceptance-policy changes.
struct InputLevelEvidence {
    struct Bucket {uint64_t samples=0,nonzero=0,badLabels=0;int32_t low=0,high=0;};
    // First complete data packet in each existing ~one-second bucket. Byte copy
    // only: all ten PCM slots and MIDI remain uninterpreted. No decoder fallback.
    struct RawPacket {
        uint64_t ordinal=0;
        uint16_t size=0;
        std::array<uint8_t,360> bytes{};
    };
    std::array<Bucket,33> buckets{};
    std::array<RawPacket,33> rawPackets{};
    std::array<uint64_t,256> input1Labels{};
    uint64_t packets=0,overflow=0;
    void observe(std::span<const uint8_t> payload) {
        const auto index=packets++/8000;
        if(index>=buckets.size()){++overflow;return;}
        auto &rawPacket=rawPackets[index];
        if(!rawPacket.size && payload.size()==rawPacket.bytes.size()) {
            rawPacket.ordinal=packets-1;
            for(size_t i=0;i<payload.size();++i)rawPacket.bytes[i]=payload[i];
            rawPacket.size=uint16_t(payload.size());
        }
        auto &b=buckets[index];
        for(size_t offset=8;offset+44<=payload.size();offset+=44) {
            ++input1Labels[payload[offset]];
            if(payload[offset]!=0x40){++b.badLabels;continue;}
            const uint32_t raw=uint32_t(payload[offset+1])<<16|
                uint32_t(payload[offset+2])<<8|payload[offset+3];
            const int32_t value=raw&0x800000?int32_t(raw)-0x1000000:int32_t(raw);
            if(!b.samples || value<b.low)b.low=value;
            if(!b.samples || value>b.high)b.high=value;
            ++b.samples;if(value)++b.nonzero;
        }
    }
};
}
