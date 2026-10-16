// SPDX-License-Identifier: Apache-2.0
#include "Pro40Replay.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
using namespace Pro40Packets;

// Independent fixture construction: wire header and non-silent PCM input.
static size_t packet(std::array<uint8_t,512> &bytes, unsigned stream,
                     uint8_t blocks, uint8_t dbc, uint16_t syt) {
    const unsigned slots = stream == 0 ? 11 : 10;
    bytes.fill(0);
    bytes[0] = 2; bytes[1] = uint8_t(slots); bytes[3] = dbc;
    bytes[4] = 0x90; bytes[5] = 2;
    bytes[6] = uint8_t(syt >> 8); bytes[7] = uint8_t(syt);
    for (unsigned f=0; f<blocks; ++f)
        for (unsigned c=0; c<slots; ++c) {
            auto p = bytes.data()+8+(f*slots+c)*4;
            p[0] = c<10 ? 0x40 : 0x80;
            p[1] = c<10 ? 0x12 : 0;
        }
    return 8+blocks*slots*4;
}

int main() {
    std::array<uint8_t,512> input{}, output{};
    // 20 seconds, independent stream cadences/phases, more than two timestamp
    // wraps and many circular-buffer/DBC wraps. Production delayed by 32 cycles.
    constexpr unsigned total=160000, lead=32, base=63990;
    for (unsigned stream=0; stream<2; ++stream) {
        ReplayQueue<> q(stream,2,1,7);
        const uint64_t rateMilli=stream==0 ? 47995200ULL : 48004800ULL;
        auto framesAt=[&](uint64_t i) { return i*rateMilli/(8000ULL*1000*8)*8; };
        auto blocksAt=[&](uint64_t i) { return uint8_t(framesAt(i+1)-framesAt(i)); };
        for (unsigned i=0; i<total+lead; ++i) {
            const auto cycle=(base+i)%cycleStampDomain;
            if (i<total) {
                const auto blocks=blocksAt(i);
                const auto p=uint32_t((i*37+stream*601)%sytDomain);
                const auto syt=blocks ? *replaySyt(p,cycle%8000) : 0xffff;
                const auto size=packet(input,stream,blocks,uint8_t(framesAt(i)),syt);
                assert(q.ingest(std::span(input).first(size),cycle,7)==ReplayError::none);
            }
            if (i==lead) assert(q.start(cycle,7)==ReplayError::none);
            if (i>=lead) {
                const auto r=q.produce(output,cycle,7); assert(r);
                const auto parsed=parse(std::span(output).first(r.bytes),playback[stream],1);
                assert(parsed && parsed.packet.blocks==blocksAt(i-lead));
                assert(parsed.packet.dbc==uint8_t(framesAt(i-lead)));
                if (parsed.packet.blocks)
                    assert(phase(parsed.packet.syt,cycle%8000)==((i-lead)*37+stream*601)%sytDomain);
                for (size_t slot=8;slot<r.bytes;slot+=4)
                    assert(output[slot+1]==0 && output[slot+2]==0 && output[slot+3]==0);
            }
            assert(q.queued()<=lead);
        }
        assert(q.queued()==0 && q.fault()==ReplayError::none);
        std::printf("Stream %u: 20-second sequence at %.1f Hz replayed without drift or data leakage\n",stream,rateMilli/1000.0);
    }
    using Q=ReplayQueue<4,2>;
    // Enable-first reception may join an already-running device. Accept every
    // initial DBC, including wrap, but never forgive gaps after the first packet.
    for(unsigned stream=0;stream<2;++stream)for(unsigned dbc=0;dbc<256;++dbc){
        for(bool cycleGap:{false,true}) {
            ReplayQueue<4,2> q(stream,2,1,7);
            auto n=packet(input,stream,8,uint8_t(dbc),0x3000);
            assert(q.ingest(std::span(input).first(n),63999,7)==ReplayError::none);
            n=packet(input,stream,8,uint8_t(dbc+8),0x4000);
            assert(q.ingest(std::span(input).first(n),cycleGap?1:0,7)==
                (cycleGap?ReplayError::receiveCycle:ReplayError::none));
        }
        ReplayQueue<4,2> q(stream,2,1,7);
        auto n=packet(input,stream,8,uint8_t(dbc),0x3000);
        assert(q.ingest(std::span(input).first(n),63999,7)==ReplayError::none);
        n=packet(input,stream,8,uint8_t(dbc+9),0x4000);
        assert(q.ingest(std::span(input).first(n),0,7)==ReplayError::continuity);
    }
    // Callback delivery is bursty even when bus cycles are regular. A bounded
    // varying batch must preserve FIFO order and the 32-cycle queue cushion.
    { ReplayQueue<> q(0,2,1,7); unsigned rx=0,tx=32;
      auto push=[&] {
          const auto n=packet(input,0,8,uint8_t(rx*8),uint16_t((rx%16)<<12));
          assert(q.ingest(std::span(input).first(n),rx%cycleStampDomain,7)==ReplayError::none);
          ++rx;
      };
      for(unsigned i=0;i<32;++i) push();
      assert(q.start(tx,7)==ReplayError::none);
      for(unsigned batch=0;batch<2000;++batch) {
          const unsigned count=1+(batch*7)%16;
          for(unsigned i=0;i<count;++i) push();
          for(unsigned i=0;i<count;++i) {
              const auto r=q.produce(output,tx%cycleStampDomain,7); assert(r);
              assert(output[3]==uint8_t((tx-32)*8)); ++tx;
          }
          assert(q.queued()==32);
      }
    }
    // Independent known replay: arrival cycle 0, SYT 0x4000; output cycle 3
    // must move SYT by three cycles to 0x7000. Failed full-data construction
    // leaves the output and queue untouched, then retries with DBC zero.
    { ReplayQueue<2,1> q(0,2,1,7);
      const auto n=packet(input,0,8,64,0x4000);
      assert(q.ingest(std::span(input).first(n),0,7)==ReplayError::none);
      assert(q.start(3,7)==ReplayError::none);
      output.fill(0xab); const auto original=output;
      assert(q.produce(std::span(output).first(423),3,7).error==ReplayError::capacity);
      assert(output==original && q.queued()==1);
      const auto r=q.produce(output,3,7);
      assert(r && r.bytes==424 && output[3]==0 && output[6]==0x70 && output[7]==0);
    }
    auto noData=[&](Q &q,unsigned cycle,unsigned generation=7) {
        return q.ingest(std::span(input).first(packet(input,0,0,0,0xffff)),cycle,generation);
    };
    { Q q(0,2,1,7);
      assert(q.start(0,7)==ReplayError::state && q.fault()==ReplayError::none);
      assert(noData(q,0)==ReplayError::none && !q.ready());
      assert(noData(q,1)==ReplayError::none && q.ready());
      assert(q.start(10,7)==ReplayError::none);
      output.fill(0xab); const auto original=output;
      assert(q.produce(std::span(output).first(7),10,7).error==ReplayError::capacity);
      assert(output==original && q.queued()==2);
      assert(q.produce(output,10,7) && q.produce(output,11,7));
      assert(q.produce(output,12,7).error==ReplayError::underrun);
      const auto stopped=output;
      assert(noData(q,2)==ReplayError::underrun);
      assert(q.produce(output,12,7).error==ReplayError::underrun && output==stopped); }
    { Q q(0,2,1,7); for(unsigned i=0;i<4;++i) assert(noData(q,i)==ReplayError::none);
      assert(noData(q,4)==ReplayError::overflow && q.queued()==4); }
    for(auto wrongCycle:{0u,2u,64000u}) {
        Q q(0,2,1,7); assert(noData(q,0)==ReplayError::none);
        assert(noData(q,wrongCycle)==ReplayError::receiveCycle);
    }
    { Q q(0,2,1,7); assert(noData(q,0,8)==ReplayError::generation); }
    { Q q(0,2,1,7); assert(noData(q,0)==ReplayError::none);
      assert(noData(q,1)==ReplayError::none); assert(q.start(10,8)==ReplayError::generation); }
    { Q q(0,2,1,7); assert(noData(q,0)==ReplayError::none);
      assert(noData(q,1)==ReplayError::none); assert(q.start(10,7)==ReplayError::none);
      assert(q.produce(output,10,8).error==ReplayError::generation); }
    { Q q(0,2,1,7); assert(noData(q,0)==ReplayError::none);
      assert(noData(q,1)==ReplayError::none); assert(q.start(10,7)==ReplayError::none);
      assert(q.produce(output,11,7).error==ReplayError::outputCycle); }
    { Q q(0,2,1,7); assert(noData(q,0)==ReplayError::none);
      const auto n=packet(input,0,8,1,0x4000);
      assert(q.ingest(std::span(input).first(n),1,7)==ReplayError::continuity); }
    { Q q(0,2,1,7); const auto n=packet(input,0,8,0,0xffff);
      assert(q.ingest(std::span(input).first(n),0,7)==ReplayError::missingSyt); }
    { Q q(0,2,1,7); const auto n=packet(input,1,8,0,0x4000);
      assert(q.ingest(std::span(input).first(n),0,7)==ReplayError::packet); }
    { Q q(2,2,1,7); assert(q.fault()==ReplayError::state); }
    { Q q(0,63,1,7); assert(q.fault()==ReplayError::state); }
    std::puts("PASS: bounded preroll, wraps, independent streams, capacity retry, sticky faults for gaps/DBC/generation/overflow/underrun");
}
