// SPDX-License-Identifier: Apache-2.0
#include "DuplexCallbackSession.hpp"
#include "SustainedInputReceiver.hpp"
#include "SustainedInputFixture.hpp"
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>
using namespace Pro40Audio;
using Published=SustainedClockPublication<>;
using R=Published::Result;
static SustainedClockPolicy policy() {
    // Simulation assumptions, NOT production accuracy/latency certification.
    return {1024,128,8,48000,1800000,2400000,4800,8400000,512*3072,500,100};
}
struct Harness {
    SustainedInputHandoff handoff{125,3};
    SustainedInputReceiver<> receiver{handoff,19};
    Fixture::Source source;
    SourceFrameClock::Identity identity{19,23,7,125,3};
    Published publication{identity,policy()};
    PlaybackPcmQueue<> output{19};
    DuplexCallbackSession<> callbacks{publication,output,{19,23,0,0},{19,23,0,2048}};
    uint64_t now=0;
    std::vector<uint64_t> expectedStampHosts;
    Harness(uint64_t epoch=19,uint64_t seed=23)
        :receiver(handoff,epoch),identity{epoch,seed,7,125,3},publication(identity,policy()),output(epoch),
         callbacks(publication,output,{epoch,seed,0,0},{epoch,seed,0,2048}) {
        assert(handoff.clock(source.clock(true)));assert(receiver.poll());
    }
    void step(unsigned agePackets=128) {
        for(unsigned n=0;n<128;++n) {
            const auto frameBus=source.nextSyt;
            const auto bytes=source.next();assert(handoff.packet(bytes,source.record.arrivalCycle,7,3));
            if(source.record.blocks && source.record.firstFrame%12000==0) {
                // Independent known generator timeline, not a fit or proposal.
                const auto elapsed=frameBus-(Fixture::Source::firstBus-3072);
                expectedStampHosts.push_back(Fixture::Source::hostOrigin+uint64_t(std::round(elapsed/1.024L/1.00004L)));
            }
        }
        // Paired observation is16ms after this consumed prefix, modeling the
        // handoff's packet age. Advance both host and controller, never just one.
        auto ahead=source;ahead.ordinal+=agePackets;
        auto pair=ahead.clock();pair.packets=uint32_t(source.ordinal);now=pair.after;
        assert(handoff.clock(pair) && receiver.poll());
    }
    R update(){return publication.update(receiver,now);}
    void prime() {
        for(unsigned n=0;n<24;++n) {step();if(update()==R::published)return;}
        assert(false);
    }
};
static void continuous(unsigned seconds) {
    auto h=std::make_unique<Harness>();
    SourceFrameClock::Stamp previous{};
    uint64_t offers=0,stamps=0,maxError=0;
    std::vector<SourceFrameClock::Stamp> pendingOracle;
    size_t checked=0;
    auto checkOracle=[&] {
        while(checked<pendingOracle.size()) {
            const auto stamp=pendingOracle[checked];
            const auto index=size_t(stamp.sample)/12000;
            if(index>=h->expectedStampHosts.size())break;
            const auto expected=h->expectedStampHosts[index];
            const auto error=stamp.host>expected?stamp.host-expected:expected-stamp.host;
            maxError=std::max(maxError,error);
            assert(error<=policy().maxCorrection);++checked;
        }
    };
    for(unsigned group=0;group<seconds*8000/128;++group) {
        if(group==120*8000/128)h->source.ticksPerFrame=512*1.0001L;
        if(group==360*8000/128)h->source.ticksPerFrame=512*0.9999L;
        h->step();checkOracle();const auto r=h->update();
        if(r==R::waiting)continue;
        if(r!=R::published)std::fprintf(stderr,"group=%u result=%u fault=%u\n",group,unsigned(r),unsigned(h->publication.fault()));
        assert(r==R::published);++offers;
        SourceFrameClock::Stamp stamp;
        assert(h->callbacks.timestamp(h->now,stamp));
        assert(stamp.seed==23 && stamp.sample>=previous.sample && stamp.host>=previous.host);
        if(stamps && stamp.sample==previous.sample)assert(stamp.host==previous.host);
        else if(stamps)assert(stamp.sample==previous.sample+12000);
        if(!stamps || stamp.sample!=previous.sample)pendingOracle.push_back(stamp);
        previous=stamp;++stamps;
        // Actual137-frame input request, nominal512; unrelated output timestamp.
        auto lease=h->publication.tryAcquire();assert(lease);
        assert(lease->metadata().evidenceFrames<=lease->next());
        const auto first=lease->next()-257;
        AudioServerPlugInIOCycleInfo cycle{};cycle.mNominalIOBufferFrameSize=512;
        cycle.mInputTime.mSampleTime=double(first);cycle.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
        cycle.mOutputTime.mSampleTime=1e9;
        std::array<float,137*8> out{};
        assert(h->callbacks.readInput(h->now,cycle,137,out));
        for(unsigned f=0;f<137;++f)for(unsigned c=0;c<8;++c)
            assert(out[f*8+c]==float(Fixture::sample(first+f,c))/8388608.f);
    }
    // A128-packet clock prefix need not end at a512-frame PCM publication.
    // The unmatched partial tail waits instead of publishing unavailable PCM.
    assert(offers>seconds*25 && previous.sample>double((seconds-1)*47000));
    // A zero timestamp can precede delivery of its corresponding input PCM
    // (our generator has16ms capture transport age). Check against the oracle
    // after those samples arrive, not by equating timestamp and receive cursors.
    h->step();h->step();checkOracle();assert(checked==pendingOracle.size());
    const auto late=h->now+policy().maxUseAge+1;
    auto untouched=previous;
    assert(!h->callbacks.timestamp(late,untouched) && untouched.sample==previous.sample && untouched.host==previous.host);
    AudioServerPlugInIOCycleInfo cycle{};std::array<float,8> silence;silence.fill(1);
    assert(!h->callbacks.readInput(late,cycle,1,silence));
    for(auto x:silence)assert(x==0);
    assert(h->publication.update(h->receiver,late)==R::stale);
    h->step();assert(h->update()==R::closed); // Cannot revive expired epoch.
    std::printf("PASS: %us native packet/rolling fit to coherent 8-channel callbacks, immutable stamps and expiry; rate changes=%u; renewal beyond 60s=%s; max oracle error=%llu host ticks (24MHz)\n",seconds,unsigned(seconds>120)+unsigned(seconds>360),seconds>60?"yes":"not exercised",static_cast<unsigned long long>(maxError));
}
static void earlyInput() {
    auto h=std::make_unique<Harness>();unsigned clipped=0,published=0;
    for(unsigned n=0;n<1000;++n) {
        h->step(0);const auto result=h->update();
        assert(result==R::waiting || result==R::published);
        if(result!=R::published)continue;
        auto lease=h->publication.tryAcquire();assert(lease);
        const auto &m=lease->metadata();++published;
        assert(m.evidenceFrames<=h->receiver.snapshotFrames());
        if(m.evidenceFrames<h->receiver.snapshotFrames())++clipped;
        assert(h->receiver.frames()==h->source.frames);
        auto first=lease->next()-128;std::array<int32_t,128*8> pcm{};
        assert(lease->read(19,first,pcm)==WindowResult::ok);
        for(unsigned f=0;f<128;++f)for(unsigned c=0;c<8;++c)
            assert(pcm[f*8+c]==Fixture::sample(first+f,c));
    }
    assert(published>400 && clipped>400 && !h->publication.failed());
    std::puts("PASS: immediate receive publication retains exact PCM; clock announces only elapsed sample prefix");
}
static void backpressure() {
    auto h=std::make_unique<Harness>();
    auto empty=h->publication.tryAcquire();assert(empty && !empty->metadata().ready);
    h->prime();auto first=h->publication.tryAcquire();assert(first);
    const auto original=first->metadata();
    for(unsigned n=0;n<4;++n) {h->step();if(h->update()==R::published)break;assert(n<3);}
    auto current=h->publication.tryAcquire();assert(current);
    const auto sequence=h->publication.sequence();
    for(unsigned n=0;n<4;++n) {
        h->step();auto r=h->update();
        if(r==R::waiting)continue;
        assert(r==R::backpressure && h->publication.sequence()==sequence);
        empty.reset();assert(h->update()==R::published);break;
    }
    assert(h->publication.sequence()>sequence);
    assert(first->metadata().evidenceSequence==original.evidenceSequence && first->next()<current->next());
    h->publication.close();assert(!h->publication.tryAcquire());
    std::array<int32_t,8> samples{};
    assert(first->read(19,first->oldest(),samples)==WindowResult::ok); // Held lease survives close.
}
static void faults() {
    for(unsigned fault=0;fault<10;++fault) {
        auto h=std::make_unique<Harness>();h->prime();
        h->step();assert(h->receiver.poll());
        NativeClockSnapshot s;assert(h->receiver.snapshot(s));
        auto pcm=h->receiver.tryAcquire();assert(pcm);
        uint64_t seq=h->receiver.clockPairs(),frames=h->receiver.snapshotFrames(),observed=s.controller.lastHost,now=h->now;
        // Ensure final512-frame publication covers the newest clock prefix.
        while(pcm->next()<frames) {
            pcm.reset();h->step();assert(h->receiver.snapshot(s));pcm=h->receiver.tryAcquire();
            seq=h->receiver.clockPairs();frames=h->receiver.snapshotFrames();observed=s.controller.lastHost;now=h->now;
        }
        switch(fault) {
        case 0:++s.generation;break;
        case 1:++s.numer;break;
        case 2:s.packet.slope=std::numeric_limits<long double>::quiet_NaN();break;
        case 3:s.bus.slope=1.01;break;
        case 4:s.maxPacketAgeTicks=policy().maxPacketAgeTicks+1;break;
        case 5:observed=now+1;break;
        case 6:seq=1;break;
        case 7:frames=SourceFrameClock::maxExactFrame;break;
        case 8:s.packet.intercept+=24576000;break; // Future model despite valid rate.
        case 9:now+=policy().maxUseAge+1;break;
        }
        const auto result=h->publication.offer(s,seq,frames,observed,pcm->copyWindow(),now);
        assert(result!=R::published && result!=R::waiting && result!=R::backpressure);
        assert(h->publication.failed() && !h->publication.tryAcquire());
        SourceFrameClock::Stamp stamp;assert(!h->callbacks.timestamp(now,stamp));
    }
    auto h=std::make_unique<Harness>();h->prime();
    auto lease=h->publication.tryAcquire();assert(lease);
    auto metadata=lease->metadata();
    assert(metadata.expiresHost<=metadata.stamp.host+policy().maxStampLag);
    assert(metadata.usable(19,23,metadata.expiresHost));
    assert(!metadata.usable(19,23,metadata.expiresHost+1));
    assert(!metadata.usable(20,23,h->now) && !metadata.usable(19,24,h->now));
}
static void output() {
    auto h=std::make_unique<Harness>();h->prime();
    uint64_t read=0;
    for(unsigned n=0;n<2000;++n) {
        std::array<float,137*6> samples;
        for(unsigned f=0;f<137;++f)for(unsigned c=0;c<6;++c)
            samples[f*6+c]=float(Fixture::sample(n*137+f,c))/8388608.f;
        AudioServerPlugInIOCycleInfo cycle{};cycle.mNominalIOBufferFrameSize=512;
        cycle.mOutputTime.mSampleTime=2048+n*137;cycle.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;
        cycle.mInputTime.mSampleTime=1e9;
        assert(h->callbacks.writeOutput(h->now,cycle,137,samples));
        while(h->output.written()-read>=8) {
            std::array<int32_t,8*6> pcm;
            assert(h->output.read(read,pcm));
            for(unsigned f=0;f<8;++f)for(unsigned c=0;c<6;++c)assert(pcm[f*6+c]==Fixture::sample(read+f,c));
            assert(h->output.commit(read,8));read+=8;
        }
    }
    assert(read==137*2000);
    AudioServerPlugInIOCycleInfo cycle{};std::array<float,6> samples{};
    assert(!h->callbacks.writeOutput(h->now+policy().maxUseAge+1,cycle,1,samples));
    assert(h->output.fault()==PlaybackQueueResult::state);
    SourceFrameClock::Stamp revoked;assert(!h->callbacks.timestamp(h->now,revoked));
    std::array<float,8> silent;silent.fill(1);
    assert(!h->callbacks.readInput(h->now,cycle,1,silent));
    for(auto sample:silent)assert(sample==0);
    // Fresh session has a distinct epoch/seed; stale mappings cannot acquire it.
    auto other=std::make_unique<Harness>(20,24);other->prime();
    DuplexCallbackSession<> wrong(other->publication,other->output,{19,23,0,0},{19,23,0,2048});
    SourceFrameClock::Stamp stamp;assert(!wrong.timestamp(other->now,stamp));
    assert(other->callbacks.timestamp(other->now,stamp) && stamp.seed==24);
    assert(other->handoff.finish() && other->receiver.poll());
    assert(other->update()==R::closed && !other->callbacks.timestamp(other->now,stamp));
    std::puts("PASS: explicit distinct input/output mappings,137-frame8/6 callbacks,8-frame native consumption and stale identity/source rejection");
}
static uint64_t callbackHost=0;
static unsigned callbackClockReads=0;
static uint64_t readCallbackClock(uint64_t)noexcept {++callbackClockReads;return callbackHost;}
static void acquiredClockTime() {
    auto h=std::make_unique<Harness>();h->prime();
    auto lease=h->publication.tryAcquire();assert(lease);
    const auto m=lease->metadata();const auto first=lease->next()-8;lease.reset();
    const auto earlier=m.publishedHost-1;
    SourceFrameClock::Stamp stamp;assert(!h->callbacks.timestamp(earlier,stamp));
    DuplexCallbackSession<> live(h->publication,h->output,{19,23,0,0},{19,23,0,2048},readCallbackClock);
    callbackHost=h->now;callbackClockReads=0;
    assert(live.timestamp(earlier,stamp) && stamp.host==m.stamp.host && callbackClockReads==1);
    AudioServerPlugInIOCycleInfo cycle{};
    cycle.mInputTime.mSampleTime=double(first);cycle.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
    cycle.mOutputTime.mSampleTime=2048;cycle.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;
    std::array<float,8*8> input{};std::array<float,8*6> output{};
    assert(live.readInput(earlier,cycle,8,input) && callbackClockReads==2);
    for(unsigned f=0;f<8;++f)for(unsigned c=0;c<8;++c)
        assert(input[f*8+c]==float(Fixture::sample(first+f,c))/8388608.f);
    assert(live.writeOutput(earlier,cycle,8,output) && callbackClockReads==3);
    // Re-sampling cannot revive stale evidence even when the caller's hint is
    // still inside its validity window. All three callback paths reject expiry.
    callbackHost=m.expiresHost+1;
    assert(!live.timestamp(h->now,stamp));
    assert(!live.readInput(h->now,cycle,8,input) && live.inputClockFailure()==16);
    assert(!live.writeOutput(h->now,cycle,8,output) && live.outputAdmissionFailure()==3);
    assert(callbackClockReads==6);
    std::puts("PASS: callback time sampled after acquisition; newer publication accepted, expired metadata still rejected");
}
int main(int argc,char **argv) {
    unsigned seconds=argc==2?unsigned(std::stoul(argv[1])):600;
    assert(seconds>=2 && seconds<=10800);
    backpressure();faults();output();acquiredClockTime();earlyInput();continuous(seconds);
}
