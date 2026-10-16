// SPDX-License-Identifier: Apache-2.0
#include "SustainedInputReceiver.hpp"
#include "SustainedInputFixture.hpp"
#include <cassert>
#include <iostream>
#include <memory>
#include <thread>
using namespace Pro40Audio;
using Queue=BasicSustainedInputHandoff<256>;
using Receiver=SustainedInputReceiver<256,1024>;
static void verify(Receiver &receiver,uint64_t expectedNext) {
    auto lease=receiver.tryAcquire();assert(lease);
    const auto next=receiver.publishedNext();assert(next==expectedNext);
    const auto frames=std::min<uint64_t>(next-8,137);
    std::array<int32_t,137*8> samples{};
    assert(lease->read(19,next-frames,std::span(samples).first(frames*8))==WindowResult::ok);
    for(unsigned f=0;f<frames;++f)for(unsigned c=0;c<8;++c)
        assert(samples[f*8+c]==Fixture::sample(next-frames+f,c));
}
int main() {
    auto q=std::make_unique<Queue>(125,3);
    auto r=std::make_unique<Receiver>(*q,19);Fixture::Source source;
    assert(q->clock(source.clock(true)));
    // >32.768s old packet-recorder limit. Poll deliberately differs from packet
    // and 512-frame boundaries. Check arbitrary callback-sized slices repeatedly.
    for(uint64_t n=0;n<72*8000+19;++n) {
        const auto payload=source.next();assert(q->packet(payload,source.record.arrivalCycle,7,3));
        if(n%128==127)assert(q->clock(source.clock()));
        if(n%64==63){assert(r->poll(256));verify(*r,std::max<uint64_t>(8,source.frames/512*512));}
    }
    assert(q->finish());assert(r->poll(256));assert(r->ended());
    assert(r->packets()==source.ordinal && r->frames()==source.frames && r->clockPairs()>4096);
    assert(r->snapshotFrames()<=r->frames());verify(*r,source.frames);
    assert(q->highWater()<=67);

    // Queue saturation and poisoned source close callback admission explicitly.
    auto full=std::make_unique<BasicSustainedInputHandoff<2>>(125,3);
    Fixture::Source f;assert(full->clock(f.clock(true)));
    auto payload=f.next();assert(full->packet(payload,f.record.arrivalCycle,7,3));
    payload=f.next();assert(!full->packet(payload,f.record.arrivalCycle,7,3));
    assert(full->fault()==SustainedInputFault::full);
    SustainedInputEvent sentinel;sentinel.packet.ordinal=123;
    assert(!full->pop(sentinel)&&sentinel.packet.ordinal==123);

    for(unsigned error=0;error<5;++error) {
        auto bad=std::make_unique<Queue>(125,3);Fixture::Source s;
        if(error!=0)assert(bad->clock(s.clock(true)));
        auto bytes=s.next();
        if(error==1) {
            assert(bad->packet(bytes,s.record.arrivalCycle,7,3)); // Initial data is deliberately not decoded.
            do {bytes=s.next();if(!s.record.blocks)assert(bad->packet(bytes,s.record.arrivalCycle,7,3));}while(!s.record.blocks);
            s.storage[8+7*44+7*4]=0; // Invalid last requested PCM label after pre-roll.
        }
        if(error==2)s.storage[3]=1; // First DBC may be arbitrary; next must match.
        const bool ok=bad->packet(bytes,s.record.arrivalCycle,error==3?8:7,3);
        if(error==2){assert(ok);bytes=s.next();assert(!bad->packet(bytes,s.record.arrivalCycle,7,3));}
        else if(error==4){assert(ok);assert(!bad->clock(s.clock(true)));}
        else assert(!ok);
        assert(bad->fault()!=SustainedInputFault::none);
    }
    // Real concurrent owners: producer acknowledges pacing from consumer only
    // in the TEST harness, never waits inside the production handoff.
    auto cq=std::make_unique<Queue>(125,3);auto cr=std::make_unique<Receiver>(*cq,19);
    std::atomic<uint64_t> produced{0},ack{0};std::atomic<bool> done{false};
    std::thread consumer([&] {
        while(!done.load(std::memory_order_acquire)) {
            const auto count=produced.load(std::memory_order_acquire);
            if(count!=ack.load()){assert(cr->poll(256));ack.store(count,std::memory_order_release);}
            else std::this_thread::yield();
        }
        assert(cr->poll(256));assert(cr->ended());
    });
    Fixture::Source concurrent;assert(cq->clock(concurrent.clock(true)));
    for(uint64_t group=1;group<=2000;++group) {
        for(unsigned n=0;n<128;++n){auto b=concurrent.next();assert(cq->packet(b,concurrent.record.arrivalCycle,7,3));}
        assert(cq->clock(concurrent.clock()));produced.store(group,std::memory_order_release);
        while(ack.load(std::memory_order_acquire)!=group)std::this_thread::yield();
    }
    assert(cq->finish());done.store(true,std::memory_order_release);consumer.join();
    verify(*cr,concurrent.frames);
    std::cout<<"PASS: sustained native-content handoff, exact 8-channel PCM through window wraps, 72 seconds, tail, concurrent owners, overflow and malformed input\n";
}
