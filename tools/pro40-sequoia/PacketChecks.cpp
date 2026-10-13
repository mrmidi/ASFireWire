// SPDX-License-Identifier: Apache-2.0
#include "Pro40Packets.hpp"
#include <cassert>
#include <cstdio>
#include <algorithm>
using namespace Pro40Packets;
int main() {
    std::array<uint8_t, 512> bytes{};
    for (auto layout : {capture[0], capture[1], playback[0], playback[1]}) {
        Am824Stream stream(layout, 1);
        auto size = stream.make(bytes, 8, 0x1234); assert(size && *size == layout.dataBytes());
        assert(read32(bytes.data()) == (0x01000000u | uint32_t(layout.slots()) << 16));
        assert(read32(bytes.data() + 4) == 0x90021234);
        for (unsigned f = 0; f < 8; ++f)
            for (unsigned slot = 0; slot < layout.slots(); ++slot)
                assert(read32(bytes.data() + 8 + (f * layout.slots() + slot) * 4) ==
                    (slot < layout.pcm ? 0x40000000u : 0x80000000u));
        auto p = parse(std::span(bytes).first(*size), layout, 1); assert(p && p.packet.blocks == 8);
        Continuity continuity; assert(continuity.accept(p.packet));
        size = stream.make(bytes, 0, 0xffff); assert(size && *size == 8);
        assert(bytes[3] == 8 && read32(bytes.data()+4) == 0x9002ffff);
        p = parse(std::span(bytes).first(*size),layout,1); assert(p && continuity.accept(p.packet));
        assert(!continuity.accept(Packet{8,9,0x1234}));
        // Failure must not advance DBC, including undersized destination and bad SYT.
        const auto saved = bytes;
        assert(!stream.make(std::span(bytes).first(10),8,0x1234)); assert(bytes == saved);
        assert(!stream.make(bytes,8,0xffff)); assert(bytes == saved);
        size=stream.make(bytes,8,0x1234); assert(size && bytes[3] == 8); // no-data/failures never advance DBC
        assert(continuity.accept(parse(std::span(bytes).first(*size),layout,1).packet));
        for (size_t cut = 0; cut < 8; ++cut) assert(!parse(std::span(bytes).first(cut),layout,1));
        assert(!parse(std::span(bytes).first(*size-4),layout,1));
        assert(!parse(std::span(bytes).first(*size),layout,2));
        bytes[8]=0; assert(parse(std::span(bytes).first(*size),layout,1).error==Error::label);
    }
    // Independent known bytes for playback stream 1 no-data (SID 1, DBS 8, DBC 0).
    {
        Am824Stream input(capture[0], 0);
        const auto size=input.make(bytes,8,0x1234);assert(size);
        // DICE receive timing ignores every possible PCM upper byte, matching
        // Linux's receive extraction. It must never relax output or MIDI checks.
        for(unsigned label=0;label<256;++label) {
            bytes[8]=uint8_t(label);
            assert(parseCaptureTiming(std::span(bytes).first(*size),0,0));
            assert(bool(parse(std::span(bytes).first(*size),capture[0],0))==(label==0x40));
        }
        bytes[8+10*4]=0;
        assert(parseCaptureTiming(std::span(bytes).first(*size),0,0).error==Error::label);
        assert(!parseCaptureTiming(std::span(bytes).first(*size),2,0));
        bytes[8+10*4]=0x80;bytes[4]=0;
        assert(parseCaptureTiming(std::span(bytes).first(*size),0,0).error==Error::header);
    }
    Am824Stream golden(playback[1],1);
    assert(golden.make(bytes,0,0xffff)==8);
    const std::array<uint8_t,8> expected={1,8,0,0,0x90,2,0xff,0xff};
    assert(std::equal(expected.begin(),expected.end(),bytes.begin()));
    // Independently specified signed 24-bit input samples; labels are validated.
    for (auto sample : {std::pair{0x40000000u,0},std::pair{0x407fffffu,8388607},
                        std::pair{0x40800000u,-8388608},std::pair{0x40ffffffu,-1}}) {
        write32(bytes.data(),sample.first);
        assert(sample24(std::span(bytes).first(4)) == sample.second);
    }
    assert(!sample24(std::span(bytes).first(3)));
    bytes[0]=0x80; assert(!sample24(std::span(bytes).first(4)));
    assert(!Am824Stream(Layout{255,1},1).make(bytes,8,0x1234));
    // Deterministic malformed-input sweep with sanitizers, including every
    // input length through maximum capacity; no read beyond supplied spans.
    uint32_t random=0x40;
    for (unsigned trial=0;trial<10000;++trial) {
        for (auto &v:bytes) { random=random*1664525u+1013904223u; v=uint8_t(random>>24); }
        (void)parse(std::span(bytes).first(trial%513),capture[0],1);
    }
    // Exhaustive cycle/SYT round trip including cycle 7999→0 and 16-cycle wrap.
    for (uint32_t cycle=0; cycle<8000; ++cycle)
        for (uint32_t offset : {0u,1u,3071u,3072u,49151u}) {
            const auto stamp=replaySyt(offset,cycle); assert(stamp);
            assert(phase(*stamp,cycle)==offset);
            const auto next=replaySyt(offset,(cycle+1)%8000); assert(next);
            const auto t0=(((*stamp>>12)*3072)+(*stamp&0xfff));
            const auto t1=(((*next>>12)*3072)+(*next&0xfff));
            assert((t1+sytDomain-t0)%sytDomain==3072);
        }
    assert(!phase(0xffff,0) && !phase(0x1c00,0) && !replaySyt(0,8000));
    // Synthetic received schedules, not hardware captures: ±100 ppm drift,
    // with both playback streams following exactly the same incoming cadence.
    for (uint64_t rateMilli : {47995200ULL,48000000ULL,48004800ULL}) {
        Am824Stream a(playback[0],1), b(playback[1],1);
        Continuity ca,cb; uint64_t frames=0, noData=0;
        for (uint64_t cycle=0;cycle<80000;++cycle) {
            const uint64_t target=(cycle+1)*rateMilli/(8000ULL*1000*8)*8;
            const uint8_t blocks=uint8_t(target-frames); assert(blocks==0||blocks==8);
            const uint16_t syt=blocks?*replaySyt(uint32_t(cycle%3072),uint32_t(cycle%8000)):0xffff;
            for (auto entry : {std::pair{&a,playback[0]},std::pair{&b,playback[1]}}) {
                const auto size=entry.first->make(bytes,blocks,syt); assert(size);
                const auto p=parse(std::span(bytes).first(*size),entry.second,1); assert(p);
                assert(p.packet.dbc==uint8_t(frames));
                assert((entry.first==&a?ca:cb).accept(p.packet));
            }
            frames+=blocks; noData+=!blocks;
        }
        assert(frames==80000*rateMilli/(8000ULL*1000*8)*8);
        std::printf("10-second simulated rate %.1f Hz: %llu frames, %llu no-data cycles, both playback streams aligned\n",
            rateMilli/1000.0,(unsigned long long)frames,(unsigned long long)noData);
    }
    std::puts("PASS: four layouts, labels, signed PCM, no-data, DBC continuity/wrap, capacity failures, malformed input, SYT wrap, ±100 ppm replay");
}
