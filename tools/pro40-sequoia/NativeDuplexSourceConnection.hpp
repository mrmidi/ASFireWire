// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuplexMachChannel.hpp"
#include "NativeStartNegotiation.hpp"
#include "PlaybackFeedSession.hpp"
#include "SustainedClockPublication.hpp"
namespace Pro40Audio {
// Non-RT source owner beside the existing SustainedInputReceiver. Caller polls
// that receiver first; no callback, DMA view or native port crosses this seam.
// Receiver and playback queue outlive this connection AND native teardown.
// The output queue starts empty. Only the host's admitted priming range supplies
// startup silence; it is transferred once, not independently recreated here.
// A hardware supervisor must arrange playback's physical first-frame relation
// and must not start consuming before primed(). This class cannot infer it.
template<class Receiver> class NativeDuplexSourceConnection {
    using Kind=DuplexMachChannel::Kind;
    using Result=DuplexMachChannel::Result;
    using Negotiation=BasicNativeStartNegotiation<NativeDuplexPacket>;
    enum class State {offer,echo,initial,commit,ready,activate,running,stopping,closed};
    std::unique_ptr<DuplexMachChannel> channel_;
    Receiver &receiver_;
    PlaybackPcmQueue<> &output_;
    PlaybackFeedSession<> outputFeed_;
    const NativeDuplexDescriptor descriptor_;
    const SustainedClockPolicy policy_;
    Negotiation negotiation_;
    State state_=State::offer;
    std::array<int32_t,4096*8> initial_{};
    std::array<uint8_t,NativeDuplexPacket::size> start_{};
    std::array<uint8_t,NativeDuplexClockPacket::size> clockBytes_{};
    std::array<uint8_t,AudioFeedPacketFor<8>::maxBytes> inputBytes_{};
    AudioFeedBatchFor<8> input_;
    size_t inputSize_=0;
    uint64_t sentInput_=0,sentClock_=0,lastNow_=0,credited_=0;
    std::optional<NativeDuplexClockMessage> clock_;
    bool inputPending_=false,failed_=false,stopRequested_=false,peerStop_=false,quiesced_=false,remoteClean_=false,closedOK_=false;
    bool fail() noexcept {failed_=true;stopRequested_=true;state_=State::stopping;return false;}
    bool send(Kind kind,std::span<const uint8_t> bytes,bool &accepted) noexcept {
        const auto r=channel_->send(kind,bytes);accepted=r==Result::ok;
        return r==Result::ok || r==Result::full ? true : fail();
    }
    bool snapshot(NativeDuplexClockMessage &m,uint64_t now) noexcept {
        NativeClockSnapshot s;
        if(!receiver_.snapshot(s) || !policy_.enough(s,receiver_.snapshotFrames()) || !policy_.acceptsSnapshot(s) ||
           s.generation!=descriptor_.clock.identity.generation || s.numer!=descriptor_.clock.identity.numer ||
           s.denom!=descriptor_.clock.identity.denom || s.controller.lastHost>now ||
           now-s.controller.lastHost>policy_.maxArrivalAge ||
           receiver_.snapshotFrames()>SourceFrameClock::maxExactFrame-policy_.lookAheadFrames)return false;
        m.nonce=channel_->nonce();m.sequence=receiver_.clockPairs();m.frames=receiver_.snapshotFrames();m.observedHost=s.controller.lastHost;
        return nativePastClockProposal(s,descriptor_.clock.identity,receiver_.snapshotFrames(),policy_.lookAheadFrames,m.clock,m.frames) &&
            policy_.enough(s,m.frames) &&
            NativeDuplexClockPacket::valid(m);
    }
    bool prepareInput(bool initial) noexcept {
        if(inputPending_)return true;
        input_.epoch=descriptor_.clock.identity.epoch;input_.firstFrame=sentInput_;
        if(initial) {
            input_.frames=std::min<uint64_t>(512,descriptor_.prefixFrames-sentInput_);
            std::copy_n(initial_.begin()+(sentInput_-descriptor_.inputFirst)*8,input_.frames*8,input_.samples.begin());
        } else {
            auto lease=receiver_.tryAcquire();if(!lease)return true;
            if(lease->oldest()>sentInput_)return fail(); // Never skip overwritten samples.
            input_.frames=std::min<uint64_t>(512,lease->next()-sentInput_)/8*8;
            if(!input_.frames)return true;
            if(lease->read(input_.epoch,sentInput_,std::span(input_.samples).first(input_.frames*8))!=WindowResult::ok)return fail();
        }
        if(AudioFeedPacketFor<8>::encode(input_,inputBytes_,inputSize_)!=AudioFeedError::ok)return fail();
        inputPending_=true;return true;
    }
    bool sendInput() noexcept {
        if(!inputPending_)return true;bool accepted=false;
        if(!send(Kind::input,std::span(inputBytes_).first(inputSize_),accepted))return false;
        if(accepted){sentInput_+=input_.frames;inputPending_=false;}return true;
    }
public:
    NativeDuplexSourceConnection(std::unique_ptr<DuplexMachChannel> channel,Receiver &receiver,PlaybackPcmQueue<> &output,
                                 const NativeDuplexDescriptor &d,SustainedClockPolicy policy,uint64_t now)
        :channel_(std::move(channel)),receiver_(receiver),output_(output),
         outputFeed_({output,d.clock.identity.epoch},d.outputFirst),descriptor_(d),policy_(policy),
         negotiation_(Negotiation::Role::source,d.clock.identity,channel_?channel_->nonce():0),
         sentInput_(d.inputFirst),sentClock_(d.evidenceSequence),lastNow_(now),credited_(d.outputFirst) {
        if(!channel_ || !channel_->healthy() || channel_->epoch()!=d.clock.identity.epoch || !policy.valid() ||
           !d.validAt(now) || d.inputChannels!=8 || d.outputChannels!=6 || d.prefixFrames-d.inputFirst>4096 ||
           (d.prefixFrames-d.inputFirst)%8 || outputFeed_.closed()){fail();return;}
        NativeDuplexClockMessage m;
        if(!snapshot(m,now) || m.sequence!=d.evidenceSequence || m.frames!=d.prefixFrames || m.observedHost!=d.evidenceHost ||
           m.clock.origin!=d.clock.origin || m.clock.ticksNumerator!=d.clock.ticksNumerator ||
           m.clock.ticksDenominator!=d.clock.ticksDenominator || m.clock.horizonFrames!=d.clock.horizonFrames){fail();return;}
        auto lease=receiver_.tryAcquire();
        if(!lease || lease->read(d.clock.identity.epoch,d.inputFirst,std::span(initial_).first((d.prefixFrames-d.inputFirst)*8))!=WindowResult::ok ||
           !negotiation_.offer(d,now,start_))fail();
    }
    bool failed() const noexcept {return failed_;}
    bool stopRequested() const noexcept {return stopRequested_;}
    bool closed() const noexcept {return state_==State::closed;}
    bool cleanupOK() const noexcept {return closed() && closedOK_ && remoteClean_;}
    // Local rights only. Caller has quiesced all users; this is not a remote
    // STOPPED receipt or proof of native teardown.
    bool closeLocal() noexcept {return !channel_ || channel_->close();}
    bool primed() const noexcept {return state_==State::running && output_.written()>=descriptor_.outputFirst+descriptor_.primedOutputFrames;}
    // Native owner has stopped independently (e.g. parent cancellation). Keep
    // servicing the peer's STOP until the bounded supervisor closes this owner.
    void requestStop() noexcept {stopRequested_=true;if(state_!=State::closed)state_=State::stopping;}
    // Supervisor only AFTER all native users are stopped/joined. Does not stop
    // hardware on its own. A negative result is sent explicitly, never masked.
    bool confirmStopped(bool clean) noexcept {
        if(!stopRequested_ || quiesced_)return false;
        quiesced_=true;remoteClean_=clean;return true;
    }
    bool poll(uint64_t now) noexcept {
        if(state_==State::closed)return !failed_;
        if(!channel_ || !channel_->healthy())return fail();
        if(now<lastNow_)return fail();lastNow_=now;
        if(state_!=State::stopping && state_!=State::running && !negotiation_.poll(now))fail();
        if(state_!=State::stopping && (receiver_.failed() || receiver_.ended() || output_.fault()!=PlaybackQueueResult::ok))fail();
        const auto flushed=state_==State::stopping?SessionResult::ok:outputFeed_.flush();
        if(flushed!=SessionResult::ok)fail();
        // Credits bound queued+in-flight playback to the remote queue capacity.
        // Therefore audio cannot block reading STOP behind a full output queue.
        DuplexMachChannel::Packet packet;
        for(unsigned n=0;n<16;++n) {
            const auto r=channel_->poll(packet);if(r==Result::idle)break;
            if(r!=Result::ok)return fail();
            if(packet.kind==Kind::stop && !packet.used){peerStop_=stopRequested_=true;state_=State::stopping;break;}
            if(state_==State::stopping)continue;
            if(packet.kind==Kind::start && state_==State::echo) {
                NativeDuplexDescriptor accepted;
                if(!negotiation_.confirm(packet.payload(),now,accepted) || accepted!=descriptor_)return fail();
                state_=State::initial;
            } else if(packet.kind==Kind::start && state_==State::ready) {
                if(!negotiation_.receiveReady(packet.payload(),now))return fail();state_=State::activate;
            } else if(packet.kind==Kind::output && state_==State::running) {
                const auto r=outputFeed_.receive(packet.payload());
                if(r!=SessionResult::ok)return fail();
            } else return fail();
        }
        bool accepted=false;
        switch(state_) {
        case State::offer:
            if(!send(Kind::start,start_,accepted))return false;if(accepted)state_=State::echo;break;
        case State::initial:
            if(sentInput_<descriptor_.prefixFrames) {
                if(!prepareInput(true) || !sendInput())return false;
            } else {
                if(!negotiation_.commit(now,start_))return fail();state_=State::commit;
            }
            break;
        case State::commit:
            if(!send(Kind::start,start_,accepted))return false;if(accepted)state_=State::ready;break;
        case State::activate:
            if(!send(Kind::activate,{},accepted))return false;if(accepted)state_=State::running;break;
        case State::running:
            if(output_.consumed()>credited_) {
                const auto consumed=output_.consumed();std::array<uint8_t,8> credit{};DuplexWire::put(credit,0,8,consumed);
                if(!send(Kind::credit,credit,accepted))return false;if(accepted)credited_=consumed;
            }
            if(!clock_ && receiver_.clockPairs()>sentClock_) {
                NativeDuplexClockMessage m;if(!snapshot(m,now))return fail();clock_=m;
                if(!NativeDuplexClockPacket::encode(m,clockBytes_))return fail();
            }
            // PCM precedes the corresponding clock proposal. Keep a pending
            // prefix under congestion rather than continually replacing it.
            // A delayed connection wake can have several complete PCM batches.
            // Sending only one per wake lets timer coalescing make the IPC
            // consumer slower than the native source. Drain bounded work while
            // preserving STOP fairness and the exact unsent batch on full.
            for(unsigned n=0;n<8;++n) {
                const auto before=sentInput_;
                if(!prepareInput(false) || !sendInput())return false;
                if(inputPending_ || sentInput_==before)break;
            }
            if(clock_ && sentInput_>=clock_->frames) {
                if(!send(Kind::clock,clockBytes_,accepted))return false;
                if(accepted){sentClock_=clock_->sequence;clock_.reset();}
            }
            break;
        case State::stopping:
            if(quiesced_ && peerStop_) {
                std::array<uint8_t,4> status{};DuplexWire::put(status,0,4,remoteClean_?1:0);
                if(!send(Kind::stopped,status,accepted))return false;
                if(accepted){closedOK_=channel_->close();state_=State::closed;}
            }
            break;
        default:break;
        }
        return !failed_;
    }
};
}
