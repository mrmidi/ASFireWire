// SPDX-License-Identifier: Apache-2.0
#include "Pro40NativeReplay.hpp"
#include "NativeInputDelivery.hpp"
#include "NativeInputFixture.hpp"
#include "SustainedInputReceiver.hpp"
#include <cassert>
#include <iostream>
#include <memory>
using namespace Pro40Audio;
int main() {
    auto handoff=std::make_unique<SustainedInputHandoff>(125,3);
    auto receiver=std::make_unique<SustainedInputReceiver<>>(*handoff,19);
    auto source=std::make_unique<Fixture::NativeSource>();
    Pro40Packets::InputMonitor monitor;
    NativeInputDelivery input(4,3,7,monitor,handoff.get());
    assert(handoff->clock(source->packets.clock(true)));
    Pro40Packets::NativeReplay replay(0,4,3,1,7,nullptr,nullptr,false);
    NativePackets::PreparedBuffer saved;uint32_t status=0,stamp=0,txCycle=918;
    std::array<uint8_t,448> output{};
    for(unsigned end=64;end<=66048;end+=64) {
        source->prepare();assert(source->append());
        assert(input.drain(source->queue.next(),7,[&](uint64_t n,auto &bytes,auto &s,auto &t) {
            return source->queue.observe(n,7,bytes,s,t);
        }));
        assert(input.next()==end && monitor.packets==end && handoff->packetCount()==end);
        assert(handoff->clock(source->packets.clock()) && receiver->poll());
        assert(receiver->packets()==end && receiver->frames()==source->packets.frames);
        // Input is readable before the first playback batch is even consumed.
        if(end==320)assert(source->queue.copied()==0 && receiver->publishedNext()>0);
        if(receiver->publishedNext()>8) {
            auto lease=receiver->tryAcquire();assert(lease);
            std::array<int32_t,128*8> pcm{};const auto first=receiver->publishedNext()-128;
            assert(lease->read(19,first,pcm)==WindowResult::ok);
            for(unsigned f=0;f<128;++f)for(unsigned c=0;c<8;++c)
                assert(pcm[f*8+c]==Fixture::sample(first+f,c));
        }
        // TX lags independent capture by256..512 cycles and frees only after
        // its entire256-packet source batch is consumed. Cross journal/timer wraps.
        const auto first=source->queue.copied();
        if(source->queue.next()-first>=512) {
            for(unsigned n=0;n<256;++n) {
                assert(source->queue.copy(first+n,7,saved,status,stamp));
                assert(replay.ingest(saved.bytes,status,stamp,7)==Pro40Packets::NativeReplayError::none);
            }
            if(!first)assert(replay.start(txCycle,7)==Pro40Packets::ReplayError::none);
            for(unsigned n=0;n<256;++n) {
                assert(replay.produce(output,txCycle,7));txCycle=(txCycle+1)%64000;
            }
            assert(source->queue.acknowledge(first,7,256));
        }
    }
    assert(source->queue.observerValid() && source->queue.maximumObserverLead()==512);
    assert(input.maximumBatch()==64 && monitor.frames==source->packets.frames);
    assert(handoff->finish() && receiver->poll() && receiver->ended());
    assert(replay.inputLevels().packets==0);
    {
        auto native=std::make_unique<Fixture::NativeSource>();
        auto q=std::make_unique<SustainedInputHandoff>(125,3);
        Pro40Packets::InputMonitor m;NativeInputDelivery delivery(4,3,7,m,q.get());
        assert(q->clock(native->packets.clock(true)));
        for(unsigned n=0;n<8;++n){native->prepare();assert(native->append());}
        auto copy=[&](uint64_t n,auto &b,auto &s,auto &t){return native->queue.observe(n,7,b,s,t);};
        assert(delivery.drain(512,7,copy) && delivery.next()==256);
        for(unsigned n=0;n<256;++n)assert(native->queue.copy(n,7,saved,status,stamp));
        assert(native->queue.acknowledge(0,7,256));
        assert(delivery.drain(512,7,copy) && delivery.next()==512 && delivery.maximumBatch()==256);
        assert(delivery.drain(512,7,copy) && q->packetCount()==512); // Empty wake does not duplicate.
        auto clock=native->packets.clock();--clock.packets;
        assert(!q->clock(clock)); // Clock must name the delivered prefix exactly.
    }
    // Corrupt a copied packet, unavailable source, wrong generation, broken
    // continuity, and bounded-queue backpressure must never look like success.
    for(unsigned fault=0;fault<9;++fault) {
        auto q=std::make_unique<SustainedInputHandoff>(125,3);
        auto native=std::make_unique<Fixture::NativeSource>();
        Pro40Packets::InputMonitor m;NativeInputDelivery delivery(4,3,7,m,q.get());
        assert(q->clock(native->packets.clock(true)));
        native->prepare();auto &r=native->group[0];
        if(fault==0)r.status=0;
        if(fault==1)r.bytes[5]=5; // channel
        if(fault==2)r.stamp|=1;
        if(fault==3)r.bytes[5]&=0x3f; // tag
        if(fault==4)native->group[2].bytes[16]=0; // PCM label after initial pre-roll.
        if(fault==5)r.bytes[11]=1; // initial DBC checked on next packet
        assert(native->append());
        const auto copy=[&](uint64_t n,auto &b,auto &s,auto &t) {
            return fault!=6 && native->queue.observe(n,7,b,s,t);
        };
        if(fault<8)assert(!delivery.drain(64,fault==7?8:7,copy));
        else {
            // No consumer: eventually fail the SPSC queue, never drop input.
            bool ok=true;
            for(unsigned group=0;ok && group<40;++group) {
                if(group){native->prepare();assert(native->append());}
                ok=delivery.drain(native->queue.next(),7,copy);
                const auto first=native->queue.copied();
                if(ok && native->queue.next()-first>=256) {
                    for(unsigned n=0;n<256;++n)assert(native->queue.copy(first+n,7,saved,status,stamp));
                    assert(native->queue.acknowledge(first,7,256));
                }
            }
            assert(!ok && q->fault()!=SustainedInputFault::none);
        }
        assert(delivery.fault()!=NativeInputDeliveryFault::none);
        if(fault==4) {
            const auto &r=delivery.rejection();
            assert(r.present && r.size==360 && r.parseError==0 && r.badPcmLabels==1);
            assert(r.handoffFault==unsigned(SustainedInputFault::packet) && r.nonzeroInvalidPcm==1);
            assert(delivery.next()==2 && r.q0==(3u<<24|11u<<16|8u)); // Original DMA fixture was already overwritten.
        }
        if(fault==5) {
            const auto &r=delivery.rejection();
            assert(r.present && r.parseError==0 && !r.badPcmLabels && delivery.next()==1);
            assert((r.previousQ0&255)==1 && (r.q0&255)==8 && r.arrival==(r.previousArrival+1)%64000);
        }
        assert(!delivery.drain(64,7,copy));
    }
    std::cout<<"PASS: early eight-channel input, independent delayed TX, exact PCM after reuse/wrap, ordered clock prefix, malformed/generation/backpressure failures\n";
}
