// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioWindowPublication.hpp"
#include "NativeClockProposal.hpp"
#include "GuardedClockPublisher.hpp"
#include "AudioCoreAudioRead.hpp"

namespace Pro40Audio {
// Explicit experimental admission limits. Tests select their own limits; this
// does not certify controller sampling, analogue latency or a HAL buffer size.
struct SustainedClockPolicy {
    uint64_t minimumFrames=0,minimumPacketPoints=0,minimumBusPoints=0;
    uint64_t lookAheadFrames=0,maxArrivalAge=0,maxUseAge=0,maxCorrection=0,maxStampLag=0;
    uint64_t maxPacketAgeTicks=0;
    double maxSampleRatePpm=0,maxBusRatePpm=0;
    bool valid() const noexcept {
        return minimumFrames && minimumPacketPoints>=3 && minimumBusPoints>=3 &&
            lookAheadFrames>=2*SourceFrameClock::period &&
            lookAheadFrames<=SourceFrameClock::maxExactFrame &&
            maxArrivalAge && maxUseAge>=maxArrivalAge && maxCorrection && maxStampLag &&
            maxPacketAgeTicks && std::isfinite(maxSampleRatePpm) && maxSampleRatePpm>0 && maxSampleRatePpm<1000000 &&
            std::isfinite(maxBusRatePpm) && maxBusRatePpm>0 && maxBusRatePpm<1000000;
    }
    bool acceptsRate(const SourceFrameClock &clock) const noexcept {
        if(!clock.valid())return false;
        const auto &id=clock.identity;
        const long double relativeRate=1000000000.L*id.denom/id.numer/48000.L*
            clock.ticksDenominator/clock.ticksNumerator;
        const long double a=maxSampleRatePpm/1000000.L,b=maxBusRatePpm/1000000.L;
        return relativeRate>=(1-a)*(1-b) && relativeRate<=(1+a)*(1+b);
    }
    bool enough(const NativeClockSnapshot &s,uint64_t frames) const noexcept {
        return frames>=minimumFrames && s.packet.count>=minimumPacketPoints && s.bus.count>=minimumBusPoints;
    }
    bool acceptsSnapshot(const NativeClockSnapshot &s) const noexcept {
        if(!std::isfinite(s.packet.slope) || !std::isfinite(s.bus.slope) || s.packet.slope<=0 || s.bus.slope<=0)return false;
        const long double rate=24576000.L/s.packet.slope;
        return std::fabs(rate/48000.L-1)*1000000<=maxSampleRatePpm &&
            std::fabs(s.bus.slope-1)*1000000<=maxBusRatePpm && s.maxPacketAgeTicks<=maxPacketAgeTicks;
    }
};
struct ClockedInputMetadata {
    SourceFrameClock::Stamp stamp{};
    uint64_t epoch=0,evidenceSequence=0,evidenceFrames=0,evidenceHost=0,publishedHost=0,expiresHost=0;
    bool ready=false;
    bool usable(uint64_t epochExpected,uint64_t seed,uint64_t now) const noexcept {
        return ready && epoch==epochExpected && stamp.seed==seed &&
            now>=publishedHost && now<=expiresHost && stamp.host<=now;
    }
};
// One non-RT publisher; many pinned readers. Timestamp and PCM share exactly
// the same publication slot. No clock estimation, IO or locks in callbacks.
// Publish only a clock prefix fully covered by the owned PCM window. Initial
// frame0 need not remain retained: actual host startup must admit its requested
// first interval separately, using an explicit input callback mapping.
template<size_t Capacity=4096,size_t Channels=8> class SustainedClockPublication {
public:
    using Window=AudioInputWindow<Capacity,Channels>;
    using Publication=AudioWindowPublication<Capacity,Channels,ClockedInputMetadata>;
    enum class Result {published,waiting,backpressure,invalid,identity,quality,stale,clock,closed};
private:
    const SourceFrameClock::Identity identity_;
    const SustainedClockPolicy policy_;
    Publication publication_;
    GuardedClockPublisher clock_;
    uint64_t lastNow_=0,lastSequence_=0,lastFrames_=0,lastEvidence_=0;
    uint64_t lastPcmNext_=0; // Publisher-only; changes only after a complete publication.
    std::optional<ClockedInputMetadata> metadata_;
    struct Candidate {NativeClockSnapshot snapshot;uint64_t sequence=0,frames=0;};
    std::optional<Candidate> pending_;
    bool failed_=false;
    Result fault_=Result::closed;
    Result fail(Result why) noexcept {failed_=true;fault_=why;publication_.close();return why;}
    bool checkTime(uint64_t now) noexcept {
        if(failed_)return false;
        if(now<lastNow_ || (lastEvidence_ && now-lastEvidence_>policy_.maxUseAge)) {
            fail(Result::stale);return false;
        }
        lastNow_=now;return true;
    }
    bool enough(const NativeClockSnapshot &s,uint64_t frames) const noexcept {
        return policy_.enough(s,frames);
    }
    bool rateOK(const NativeClockSnapshot &s) const noexcept {
        return policy_.acceptsSnapshot(s);
    }
public:
    SustainedClockPublication(SourceFrameClock::Identity id,SustainedClockPolicy policy)
        :identity_(id),policy_(policy),publication_(Window(id.epoch,0)),
         clock_(policy.maxCorrection,policy.maxArrivalAge,policy.maxUseAge) {
        if(!id.epoch || !id.seed || !id.numer || !id.denom || !policy.valid())fail(Result::invalid);
    }
    auto tryAcquire() noexcept {return publication_.tryAcquire();}
    bool failed() const noexcept {return failed_;} // Publisher only or after quiescence.
    Result fault() const noexcept {return fault_;}
    uint64_t sequence() const noexcept {return lastSequence_;}
    uint64_t pcmNext() const noexcept {return lastPcmNext_;} // Publisher only.
    void close() noexcept {fail(Result::closed);}
    Result poll(uint64_t now) noexcept {
        if(failed_)return Result::closed;
        return checkTime(now)?Result::waiting:fault_;
    }
    Result offer(const NativeClockSnapshot &s,uint64_t sequence,uint64_t frames,
                 uint64_t observed,const Window &window,uint64_t now) noexcept {
        if(failed_)return Result::closed;
        if(!checkTime(now))return fault_;
        if(window.epoch()!=identity_.epoch || s.generation!=identity_.generation || s.numer!=identity_.numer || s.denom!=identity_.denom)
            return fail(Result::identity);
        if(!sequence || !frames || frames%8 || sequence<lastSequence_ || frames<lastFrames_ ||
           !observed || observed>now || observed<s.controller.lastHost ||
           frames>SourceFrameClock::maxExactFrame-policy_.lookAheadFrames)
            return fail(Result::invalid);
        if(now-observed>policy_.maxArrivalAge)return fail(Result::stale);
        if(!enough(s,frames))return Result::waiting;
        if(!rateOK(s))return fail(Result::quality);
        SourceFrameClock proposal;
        if(!nativeClockProposal(s,identity_,frames+policy_.lookAheadFrames,proposal))return fail(Result::clock);
        return offerProposal(proposal,sequence,frames,observed,window,now);
    }
    // Receiver-side rational proposal, after the authenticated source applies
    // native fit quality checks. No ABI-dependent floats cross IPC. The receiver
    // still checks identity, sane rate/horizon, freshness, monotonic evidence and
    // bounded timestamp changes; this does not certify the source estimator.
    Result offerProposal(const SourceFrameClock &proposal,uint64_t sequence,uint64_t frames,
                         uint64_t observed,const Window &window,uint64_t now) noexcept {
        if(failed_)return Result::closed;
        if(!checkTime(now))return fault_;
        if(window.epoch()!=identity_.epoch || proposal.identity!=identity_)return fail(Result::identity);
        if(!sequence || !frames || frames%8 || sequence<lastSequence_ || frames<lastFrames_ ||
           !observed || observed>now || frames>SourceFrameClock::maxExactFrame-policy_.lookAheadFrames)
            return fail(Result::invalid);
        if(now-observed>policy_.maxArrivalAge)return fail(Result::stale);
        if(sequence==lastSequence_) {
            if(observed!=lastEvidence_ || frames!=lastFrames_)return fail(Result::invalid);
            return Result::waiting;
        }
        if(frames<policy_.minimumFrames)return Result::waiting;
        if(!proposal.valid() || proposal.horizonFrames!=frames+policy_.lookAheadFrames)return fail(Result::clock);
        if(!policy_.acceptsRate(proposal))return fail(Result::quality);
        if(window.next()<frames)return Result::waiting;
        if(window.oldest()>=frames)return fail(Result::stale);
        uint64_t lastHost=0,endHost=0;
        if(!proposal.hostTime(identity_,frames-1,lastHost) || lastHost>observed ||
           !proposal.hostTime(identity_,proposal.horizonFrames,endHost) || now>endHost ||
           observed>UINT64_MAX-policy_.maxUseAge)return fail(Result::clock);
        // Speculative copy: an unavailable publication slot must not consume a
        // clock boundary or evidence sequence. Retry remains explicit/bounded.
        auto trial=clock_;
        using R=GuardedClockPublisher::Result;
        if(trial.offer(proposal,sequence,observed,now)!=R::ready)return fail(Result::clock);
        SourceFrameClock::Stamp stamp;
        const auto advanced=trial.advance(now,stamp);
        if(advanced==R::waiting) {
            if(!trial.lastCommitted(stamp))return Result::waiting;
        } else if(advanced!=R::published)return fail(Result::clock);
        if(stamp.host>now || now-stamp.host>policy_.maxStampLag)return fail(Result::stale);
        const auto stampExpiry=stamp.host>UINT64_MAX-policy_.maxStampLag?UINT64_MAX:
            stamp.host+policy_.maxStampLag;
        const ClockedInputMetadata metadata{stamp,identity_.epoch,sequence,frames,observed,now,
            std::min({observed+policy_.maxUseAge,endHost,stampExpiry}),true};
        const auto published=publication_.tryPublish(window,metadata);
        if(published==PublicationResult::noSlot)return Result::backpressure;
        if(published!=PublicationResult::ok)return fail(Result::invalid);
        clock_=trial;lastSequence_=sequence;lastFrames_=frames;lastEvidence_=observed;
        metadata_=metadata;lastPcmNext_=window.next();
        return Result::published;
    }
    // Publish newly received PCM between clock observations. Reuse the exact
    // admitted metadata: PCM arrival is not evidence of a new clock, timestamp,
    // horizon or expiry. Existing pinned readers retain their coherent slot.
    Result refreshPcm(const Window &window,uint64_t now) noexcept {
        if(failed_)return Result::closed;
        if(!checkTime(now))return fault_;
        if(window.epoch()!=identity_.epoch)return fail(Result::identity);
        if(!metadata_)return Result::waiting; // Cannot admit a session from PCM alone.
        if(!metadata_->usable(identity_.epoch,identity_.seed,now))return fail(Result::stale);
        if(window.next()<lastPcmNext_ || window.oldest()>lastPcmNext_ ||
           window.next()>SourceFrameClock::maxExactFrame)return fail(Result::invalid);
        if(window.next()==lastPcmNext_)return Result::waiting;
        const auto result=publication_.tryPublish(window,*metadata_);
        if(result==PublicationResult::noSlot)return Result::backpressure;
        if(result!=PublicationResult::ok)return fail(Result::invalid);
        lastPcmNext_=window.next();return Result::published;
    }
    // Same-owner adapter for the existing sustained native receiver. Snapshot
    // counts are retained fit-window sizes; clockPairs/snapshotFrames carry the
    // cumulative progress. Never use fit.count as elapsed session progress.
    template<class Receiver> Result update(Receiver &receiver,uint64_t now) noexcept {
        if(failed_)return Result::closed;
        if(!checkTime(now))return fault_;
        if(receiver.failed() || receiver.ended())return fail(Result::closed);
        NativeClockSnapshot snapshot;
        if(!receiver.snapshot(snapshot))return Result::waiting;
        SourceFrameClock proposal;uint64_t pastFrames=0;
        if(!nativePastClockProposal(snapshot,identity_,receiver.snapshotFrames(),policy_.lookAheadFrames,proposal,pastFrames))
            return fail(Result::clock);
        if(!enough(snapshot,pastFrames))return Result::waiting;
        const Candidate latest{snapshot,receiver.clockPairs(),pastFrames};
        // A partial PCM batch can trail the clock prefix. Keep that proposal
        // until its data is published; continually replacing it with the latest
        // prefix can starve publication when rate drift shifts batch alignment.
        if(!pending_ && latest.sequence>lastSequence_)pending_=latest;
        if(!pending_)return Result::waiting;
        if(!enough(pending_->snapshot,pending_->frames))pending_=latest;
        auto lease=receiver.tryAcquire();
        if(!lease)return Result::backpressure;
        const auto result=offer(pending_->snapshot,pending_->sequence,pending_->frames,
            pending_->snapshot.controller.lastHost,lease->copyWindow(),now);
        if(result==Result::published) {
            pending_.reset();
            if(latest.sequence>lastSequence_)pending_=latest;
        }
        return result;
    }
};
}
