// SPDX-License-Identifier: Apache-2.0
#pragma once
// Test-only generated packets/clock. These ages are NOT live device settings.
#include "SustainedInputReceiver.hpp"
#include "SustainedInputFixture.hpp"
#include "NativeDuplexPacket.hpp"
#include "SustainedClockPublication.hpp"
#include <mach/mach_time.h>
#include <cassert>
namespace Pro40Audio {
struct DuplexGeneratedSource {
    mach_timebase_info_data_t tb{};
    uint64_t base=0,now=0;
    std::unique_ptr<SustainedInputHandoff> handoff;
    std::unique_ptr<SustainedInputReceiver<>> receiver;
    Fixture::Source packets;
    uint64_t ticks(uint64_t ns)const{return uint64_t(__uint128_t(ns)*tb.denom/tb.numer);}
    ControllerClockPair pair(bool first=false) {
        auto p=packets.clock(first);
        // Shift/scale the independent 24MHz host fixture into this process's
        // real Mach timebase. No recorded audio or hardware calls.
        const auto convert=[&](uint64_t v){return base+uint64_t(__uint128_t(v-Fixture::Source::hostOrigin)*125*tb.denom/(3*tb.numer));};
        // First pair's lower uncertainty bound predates its origin by50ticks.
        const auto mid=convert(p.uptime);p.uptime=mid;p.before=mid-ticks(3000);p.after=mid+ticks(3000);return p;
    }
    DuplexGeneratedSource() {
        assert(mach_timebase_info(&tb)==KERN_SUCCESS);base=mach_absolute_time()-ticks(280000000);
        handoff=std::make_unique<SustainedInputHandoff>(tb.numer,tb.denom);
        receiver=std::make_unique<SustainedInputReceiver<>>(*handoff,19);
        assert(handoff->clock(pair(true)) && receiver->poll());for(unsigned n=0;n<16;++n)step();
    }
    void step() {
        for(unsigned n=0;n<128;++n){const auto bytes=packets.next();assert(handoff->packet(bytes,packets.record.arrivalCycle,7,3));}
        const auto saved=packets.ordinal;packets.ordinal+=128;auto p=pair();packets.ordinal=saved;p.packets=uint32_t(saved);
        now=p.after;assert(handoff->clock(p) && receiver->poll());
    }
    // Deliberately roomy test-only ages: sanitizer work is not a live latency
    // measurement. Source-loss checks still require expiry within these bounds.
    SustainedClockPolicy policy()const {
        return {1024,128,8,48000,ticks(500000000),ticks(750000000),ticks(200000),ticks(1000000000),512*3072,500,100};
    }
    NativeDuplexDescriptor descriptor(uint64_t seed=1) {
        NativeClockSnapshot s;assert(receiver->snapshot(s));NativeDuplexDescriptor d;
        assert(nativeClockProposal(s,{19,seed,7,tb.numer,tb.denom},receiver->snapshotFrames()+policy().lookAheadFrames,d.clock));
        d.evidenceSequence=receiver->clockPairs();d.prefixFrames=receiver->snapshotFrames();d.evidenceHost=s.controller.lastHost;
        d.preparedHost=mach_absolute_time();d.expiresHost=d.preparedHost+ticks(150000000);
        auto input=receiver->tryAcquire();assert(input);d.inputFirst=input->oldest();d.initialInputFrames=512;
        d.outputFirst=0;d.primedOutputFrames=512;d.inputTimeline={19,seed,0,0};d.outputTimeline={19,seed,0,0};
        assert(d.validAt(d.preparedHost));return d;
    }
};
}
