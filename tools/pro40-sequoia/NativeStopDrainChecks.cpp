// SPDX-License-Identifier: Apache-2.0
#include "NativeStopDrain.hpp"
#include "ReceiveBankHandoffs.hpp"
#include "NativeStoppedPrefix.hpp"
#include "EarlyTransmitGate.hpp"
#include <cassert>
#include <cstdio>
using namespace NativePackets;
int main() {
    // Every bank phase retains an exact terminal cut. Pending/failed Notify
    // cannot supply a target. No padding, early stop or quiet-time assumption.
    ReceiveBankHandoffs<256,1024> graph;
    assert(graph.terminalProgress()==512);
    assert(graph.begin(257) && !graph.terminalProgress());
    assert(graph.finish(true) && graph.terminalProgress()==768);
    assert(graph.begin(513) && graph.finish(true) && graph.terminalProgress()==1024);
    assert(!graph.begin(800) && graph.terminalProgress()==1024);
    ReceiveBankHandoffs<256,1024> failed;
    assert(failed.begin(257) && !failed.finish(false) && !failed.terminalProgress());
    assert(!failed.stopBeforePublication() && !failed.terminalProgress());
    // Reproduce l3ooYj: Stop arrives in the early TX timer gate after65
    // publications. That gate is read-only; all four old tails must still drain.
    // Exercise Stop before/after a timer read and during final revalidation.
    for(unsigned stopGuard:{1u,2u,33u,34u,35u}) {
        ReceiveBankHandoffs<256,65536> tx;
        for(unsigned i=0;i<65;++i)assert(tx.begin((i+1)*256+1) && tx.finish(true));
        assert(tx.begin(16897));
        unsigned lead=256,guards=0;EarlyTransmitGate gate;
        gate.wait(lead,[&]{return ++guards==stopGuard?-7:0;},
            [](unsigned &){return 0;},[]{return 0;},[]{},255);
        assert(gate.result==EarlyTransmitGate::Result::guardFailure);
        assert(tx.stopBeforePublication() && !tx.fault() && tx.completed()==65);
        assert(tx.terminalProgress()==17152 && !tx.begin(16897));
        std::array<uint64_t,4> targets{17664,17664,*tx.terminalProgress(),17152};
        std::array<uint64_t,4> progress{17280,17280,16897,16897};
        unsigned now=0;
        assert(drainPublishedStops(targets,150u,[&]{return now;},[&](auto &p){p=progress;return true;},[&] {
            ++now;for(unsigned n=0;n<4;++n)progress[n]=std::min(targets[n],progress[n]+64);
        })==StopDrainResult::complete);
        assert(!tx.finish(true) && tx.fault() && !tx.terminalProgress());
    }
    unsigned cases=0;
    for(unsigned phase=0;phase<256;++phase) {
        constexpr std::array<uint64_t,4> targets{1280,1280,1024,1024};
        std::array<uint64_t,4> progress{768+phase/64*64,768+phase/64*64,512+phase,512+phase};
        unsigned now=0;
        const auto r=drainPublishedStops(targets,150u,[&]{return now;},[&](auto &p){p=progress;return true;},[&] {
            ++now;
            for(unsigned i=0;i<4;++i)if(progress[i]<targets[i] && (i<2?now%8==0:now%2==0))
                progress[i]=std::min(targets[i],progress[i]+(i<2?64u:16u));
        });
        assert(r==StopDrainResult::complete && progress==targets && now<=64);
        for(unsigned i=0;i<4;++i) {
            StoppedPrefix p{progress[i],progress[i],progress[i]/(i<2?64:1),4096,{i<2?64u:1u},unsigned(targets[i]/256-2),false,256};
            assert(p.valid());
        }
        ++cases;
    }
    const std::array<uint64_t,1> target{512};unsigned now=0,calls=0;
    assert(drainPublishedStops(target,10u,[&]{return now;},[&](auto &p){p={511};return true;},[&]{++now;})==StopDrainResult::timeout && now==10);
    for(unsigned failure=0;failure<3;++failure) {
        calls=0;
        assert(drainPublishedStops(target,10u,[]{return 0u;},[&](auto &p) {
            p={failure==0?513u: calls++?510u:511u};return failure!=2;
        },[]{})==StopDrainResult::invalid);
    }
    assert(drainPublishedStops(target,0u,[]{return 0u;},[](auto &p){p={512};return true;},[]{assert(false);})==StopDrainResult::complete);
    assert(drainPublishedStops(std::array<uint64_t,1>{0},10u,[]{return 0u;},[](auto &){assert(false);return true;},[]{assert(false);})==StopDrainResult::invalid);
    std::printf("PASS: %u bank phases, independent RX/TX completion, exact endpoints, stopped prefixes, pending/failed graph, timeout, stale/backwards/overrun and already-drained checks\n",cases);
}
