// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeDuplexPacket.hpp"
#include "DuplexCallbackSession.hpp"
#include "AudioFeedSession.hpp"
namespace Pro40Audio {
// Receiver resources for a single admitted session. One non-RT connection owner
// calls receiveInput/receiveClock/pump/activate/close. Concurrent callback users
// only use ready/timestamp/readInput/writeOutput. One serialized output producer,
// one playback consumer. The connection owner must join before reclamation.
template<size_t Inputs=8,size_t Outputs=6> class DuplexSessionBuffers {
    // Keep input callback history independently of native transfer batches.
    // A 3,072-frame startup cushion in a 4,096-frame window left only 1,024
    // frames for a late callback. Generated process checks exceeded that
    // margin. 8,192 retains slack on both sides; this is not a HAL latency claim.
    static constexpr size_t inputWindowFrames=8192;
    using Publication=SustainedClockPublication<inputWindowFrames,Inputs>;
    using Result=typename Publication::Result;
    AudioFeedSession<inputWindowFrames,Inputs> feed_;
    mutable Publication published_;
    mutable PlaybackPcmQueue<8192,Outputs> playback_;
    mutable DuplexCallbackSession<inputWindowFrames,Inputs,Outputs> callbacks_;
    std::optional<AudioInputWindow<inputWindowFrames,Inputs>> publishing_; // Owner-only fixed storage.
    const SustainedClockPolicy policy_;
    std::optional<NativeDuplexClockMessage> pending_,latest_;
    uint64_t receivedSequence_=0,receivedFrames_=0,receivedHost_=0;
    mutable std::atomic<bool> stopped_{false};
    std::atomic<bool> failed_{false},activated_{false};
    bool closed_=false;
    bool fail() noexcept {failed_.store(true,std::memory_order_release);close();return false;}
    bool allowed() const noexcept {
        return !stopped_.load(std::memory_order_acquire) && !failed_.load(std::memory_order_acquire);
    }
public:
    const NativeDuplexDescriptor descriptor;
    const uint64_t nonce;
    DuplexSessionBuffers(const NativeDuplexDescriptor &d,uint64_t n,SustainedClockPolicy policy,
        typename DuplexCallbackSession<inputWindowFrames,Inputs,Outputs>::HostClock hostClock=
            DuplexCallbackSession<inputWindowFrames,Inputs,Outputs>::suppliedHostTime)
        :feed_(d.clock.identity.epoch,d.inputFirst),published_(d.clock.identity,policy),
         playback_(d.clock.identity.epoch,d.outputFirst),callbacks_(published_,playback_,d.inputTimeline,d.outputTimeline,hostClock),
         policy_(policy),descriptor(d),nonce(n) {
        if(!n || !d.validAt(d.preparedHost) || d.inputChannels!=Inputs || d.outputChannels!=Outputs || !policy.valid()) {
            fail();return;
        }
        receivedSequence_=d.evidenceSequence;receivedFrames_=d.prefixFrames;receivedHost_=d.evidenceHost;
        pending_=NativeDuplexClockMessage{n,d.clock,d.evidenceSequence,d.prefixFrames,d.evidenceHost};
        // Explicit startup silence, sized by the admitted descriptor. No native
        // consumer may run against this queue before activation. Future worker
        // integration must preserve/transfer this exact primed range once.
        std::array<int32_t,4096*Outputs> zero{};
        for(uint64_t frame=0;frame<d.primedOutputFrames;) {
            const auto count=std::min<uint64_t>(4096,d.primedOutputFrames-frame);
            if(playback_.write(d.clock.identity.epoch,d.outputFirst+frame,std::span(zero).first(count*Outputs))!=PlaybackQueueResult::ok) {
                fail();return;
            }
            frame+=count;
        }
    }
    // Owner-only. False is terminal; PCM pending publication is handled inside
    // pump. There are no callback readers of feed_, only published_.
    bool receiveInput(std::span<const uint8_t> bytes,uint64_t now) noexcept {
        if(!allowed())return false;
        const auto r=feed_.receive(bytes);
        if(r!=SessionResult::ok && r!=SessionResult::pending)return fail();
        return pump(now);
    }
    bool receiveClock(std::span<const uint8_t> bytes,uint64_t now) noexcept {
        NativeDuplexClockMessage m;
        if(!allowed() || !NativeDuplexClockPacket::decode(bytes,m) || m.nonce!=nonce ||
           m.clock.identity!=descriptor.clock.identity || m.sequence<=receivedSequence_ ||
           m.frames<receivedFrames_ || m.observedHost<receivedHost_ || m.observedHost>now ||
           m.frames>SourceFrameClock::maxExactFrame-policy_.lookAheadFrames ||
           m.clock.horizonFrames!=m.frames+policy_.lookAheadFrames || !policy_.acceptsRate(m.clock) ||
           now-m.observedHost>policy_.maxArrivalAge)return fail();
        receivedSequence_=m.sequence;receivedFrames_=m.frames;receivedHost_=m.observedHost;
        latest_=m;if(!pending_)pending_=m;
        return pump(now);
    }
    bool pump(uint64_t now) noexcept {
        if(!allowed()){close();return false;}
        if(playback_.fault()!=PlaybackQueueResult::ok || published_.poll(now)!=Result::waiting)return fail();
        const auto flushed=feed_.flush();
        if(flushed!=SessionResult::ok && flushed!=SessionResult::pending)return fail();
        auto lease=feed_.tryAcquire();if(!lease)return true;
        if(!pending_ && lease->next()==published_.pcmNext())return true;
        lease->copyWindowInto(publishing_);
        if(pending_) {
            const auto &p=*pending_;
            const auto r=published_.offerProposal(p.clock,p.sequence,p.frames,p.observedHost,*publishing_,now);
            if(r==Result::backpressure)return true;
            if(r==Result::published) {
                const auto sequence=p.sequence;pending_.reset();
                if(latest_ && latest_->sequence>sequence)pending_=latest_;
                return true;
            }
            if(r!=Result::waiting)return fail();
        }
        // Clock traffic and PCM delivery have independent cadences. A pending
        // future prefix (or no new clock) must not hide already received PCM.
        // This preserves the old timestamp and expiry, including on retries.
        const auto r=published_.refreshPcm(*publishing_,now);
        return r==Result::published || r==Result::waiting || r==Result::backpressure ? true : fail();
    }
    // Coherent startup check, callable by the control owner. It never consumes
    // input/output. Startup deadline cannot be extended by later clock packets.
    bool admissible(uint64_t now) const noexcept {
        if(!allowed() || !descriptor.validAt(now) || playback_.fault()!=PlaybackQueueResult::ok ||
           !nonce)return false;
        auto lease=published_.tryAcquire();
        if(!lease || lease->oldest()>descriptor.inputFirst ||
           lease->next()<descriptor.inputFirst+descriptor.initialInputFrames)return false;
        const auto m=lease->metadata();
        return m.usable(descriptor.clock.identity.epoch,descriptor.clock.identity.seed,now) &&
            m.evidenceSequence>=descriptor.evidenceSequence && m.evidenceFrames>=descriptor.prefixFrames;
    }
    bool prepared(uint64_t now) const noexcept {
        return playback_.consumed()==descriptor.outputFirst &&
            playback_.written()==descriptor.outputFirst+descriptor.primedOutputFrames && admissible(now);
    }
    // Supervisor calls only after both endpoints accepted activation. Producing
    // READY bytes or a valid descriptor alone must never call this method.
    bool activate(uint64_t now) noexcept {
        if(activated_.load(std::memory_order_relaxed) || !prepared(now))return false;
        activated_.store(true,std::memory_order_release);return true;
    }
    bool ready(uint64_t now) const noexcept {return activated_.load(std::memory_order_acquire) && prepared(now);}
    bool failed() const noexcept {return failed_.load(std::memory_order_acquire);}
    struct Diagnostic {bool failed;PlaybackQueueResult output;Result clock;uint64_t sequence;uint32_t outputAdmission;uint32_t inputFailure=0,inputClockFailure=0;uint64_t inputOldest=0,inputNext=0;};
    Diagnostic ownerDiagnostic() const noexcept { // Connection owner or after it joins.
        return {failed(),playback_.fault(),published_.failed()?published_.fault():Result::waiting,published_.sequence(),callbacks_.outputAdmissionFailure(),callbacks_.inputFailure(),callbacks_.inputClockFailure(),callbacks_.inputOldest(),callbacks_.inputNext()};
    }
    uint64_t publishedClockSequence() const noexcept {return published_.sequence();} // Connection owner only.
    void requestStop() const noexcept {stopped_.store(true,std::memory_order_release);}
    void close() noexcept { // Connection owner, or control after that owner joins.
        requestStop();if(closed_)return;closed_=true;feed_.close();published_.close();
    }
    bool timestamp(uint64_t now,SourceFrameClock::Stamp &out) const noexcept {
        return allowed() && activated_.load(std::memory_order_acquire) && callbacks_.timestamp(now,out);
    }
    bool readInput(uint64_t now,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<float> out) const noexcept {
        if(!allowed() || !activated_.load(std::memory_order_acquire)){std::fill(out.begin(),out.end(),0);return false;}
        return callbacks_.readInput(now,cycle,frames,out);
    }
    bool writeOutput(uint64_t now,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<const float> out) const noexcept {
        return allowed() && activated_.load(std::memory_order_acquire) && callbacks_.writeOutput(now,cycle,frames,out);
    }
    bool readPlayback(uint64_t first,std::span<int32_t> out) noexcept {
        return allowed() && activated_.load(std::memory_order_acquire) && playback_.read(first,out);
    }
    bool commitPlayback(uint64_t first,size_t frames) noexcept {
        return allowed() && activated_.load(std::memory_order_acquire) && playback_.commit(first,frames);
    }
    // Connection consumer only. No data yet is normal for IPC, not a native
    // underrun. Copy only complete eight-frame groups and hold until send accepts.
    // A partial callback tail remains queued for the next callback; never pad it.
    bool copyPlaybackBatch(AudioFeedBatchFor<Outputs> &batch,size_t maxFrames=512) noexcept {
        if(!allowed() || !activated_.load(std::memory_order_acquire))return false;
        const auto first=playback_.consumed();
        const auto frames=std::min<uint64_t>({512,maxFrames,playback_.written()-first})/8*8;
        if(!frames)return false;
        batch.epoch=descriptor.clock.identity.epoch;batch.firstFrame=first;batch.frames=frames;
        return playback_.read(first,std::span(batch.samples).first(frames*Outputs));
    }
};
}
