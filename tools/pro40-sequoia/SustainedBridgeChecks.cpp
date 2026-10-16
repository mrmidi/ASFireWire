// SPDX-License-Identifier: Apache-2.0
#include "SustainedInputBridge.hpp"
#include "SustainedInputFixture.hpp"
#include <cassert>
#include <iostream>
#include <memory>
using namespace Pro40Audio;
int main() {
    // No hardware: real consumer lifecycle and a paced input source. Starts
    // with a queue larger than any producer burst; finish drains the exact tail.
    auto bridge=std::make_unique<SustainedInputBridge>(125,3,19);
    Fixture::Source source;assert(bridge->start());assert(!bridge->start());
    assert(bridge->handoff.clock(source.clock(true)));
    for(unsigned group=0;group<200;++group) {
        for(unsigned n=0;n<128;++n){auto p=source.next();assert(bridge->handoff.packet(p,source.record.arrivalCycle,7,3));}
        assert(bridge->handoff.clock(source.clock()));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(bridge->finish(true));assert(!bridge->finish(true));
    assert(bridge->receiver.frames()==source.frames);
    {
        auto lease=bridge->receiver.tryAcquire();assert(lease);
        std::array<int32_t,8> frame{};
        assert(lease->read(19,source.frames-1,frame)==WindowResult::ok);
        for(unsigned c=0;c<8;++c)assert(frame[c]==Fixture::sample(source.frames-1,c));
    }
    // Before-anchor failure and destruction while idle both join their owner.
    bridge=std::make_unique<SustainedInputBridge>(125,3,19);
    assert(bridge->start());assert(!bridge->finish(false));
    bridge=std::make_unique<SustainedInputBridge>(125,3,19);assert(bridge->start());bridge.reset();

    // Three pinned publications force pending. Queue remains ordered; releasing
    // one pin publishes precisely the pending batch, without resend or overwrite.
    auto q=std::make_unique<BasicSustainedInputHandoff<256>>(125,3);
    auto r=std::make_unique<SustainedInputReceiver<256,1024>>(*q,19);
    Fixture::Source pinned;assert(q->clock(pinned.clock(true)));
    auto first=r->tryAcquire();assert(first);
    auto advance=[&](uint64_t target) {
        while(pinned.frames<target){auto p=pinned.next();assert(q->packet(p,pinned.record.arrivalCycle,7,3));}
        assert(q->clock(pinned.clock()));assert(r->poll(256));
    };
    advance(512);auto second=r->tryAcquire();assert(second && second->next()==512);
    advance(1024);auto third=r->tryAcquire();assert(third && third->next()==1024);
    advance(1536);assert(r->publishedNext()==1024);
    assert(first->next()==8 && second->next()==512 && third->next()==1024);
    first.reset();assert(r->poll(256));assert(r->publishedNext()==1536);
    // Hold the remaining old snapshots and the newly published one. Sustained
    // pressure eventually fills the queue and closes NEW reader admission.
    auto fourth=r->tryAcquire();assert(fourth);
    advance(2048);assert(r->publishedNext()==1536);
    bool full=false;
    for(unsigned n=0;n<256;++n){auto p=pinned.next();if(!q->packet(p,pinned.record.arrivalCycle,7,3)){full=true;break;}}
    assert(full && !r->poll() && !r->tryAcquire());
    std::array<int32_t,8> retained{};
    assert(second->read(19,0,retained)==WindowResult::stale);
    assert(second->read(19,8,retained)==WindowResult::ok);
    for(unsigned c=0;c<8;++c)assert(retained[c]==Fixture::sample(8,c));
    second.reset();third.reset();fourth.reset();
    std::cout<<"PASS: real consumer thread drain/cancel/join, pinned readers, ordered pending publication, queue overflow closes admission without overwriting leases\n";
}
