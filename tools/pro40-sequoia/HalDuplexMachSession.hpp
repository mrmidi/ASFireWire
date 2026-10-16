// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "HalDuplexDriverState.hpp"
#include "DuplexSessionBuffers.hpp"
#include "DuplexMachChannel.hpp"
#include "NativeStartNegotiation.hpp"
#include "AudioThreadScheduling.hpp"
#include <chrono>
#include <exception>
#include <pthread.h>
#include <thread>
namespace Pro40Audio {
// Concrete HAL-side connection owner. Preparation and all Mach IO run off the
// audio callbacks. Owns ports, buffers and one joined thread. Endpoint/code
// authorization and remote process supervision must precede prepare(); neither
// a guessed PID nor a token from an incoming audio message is authorization.
class HalDuplexMachSession final:public HalDuplexSession {
    using Wall=std::chrono::steady_clock;
    using Kind=DuplexMachChannel::Kind;
    using Result=DuplexMachChannel::Result;
    using Negotiation=BasicNativeStartNegotiation<NativeDuplexPacket>;
    mutable DuplexSessionBuffers<> buffers_;
    std::unique_ptr<DuplexMachChannel> channel_;
    const std::array<uint8_t,NativeDuplexPacket::size> offer_;
    const Wall::time_point startupDeadline_;
    const std::chrono::milliseconds stopBudget_;
    pthread_t thread_{};
    mutable std::atomic<bool> stop_{false};
    std::atomic<bool> ready_{false},done_{false};
    bool started_=false,joined_=false,cleanupOK_=false,localClosed_=false;
    DuplexSessionBuffers<>::Diagnostic diagnostic_{};
    AudioThreadScheduling scheduling_{};
    static void pause(){std::this_thread::sleep_for(std::chrono::microseconds(100));}
    HalDuplexMachSession(const NativeDuplexMessage &offer,std::unique_ptr<DuplexMachChannel> channel,
                        SustainedClockPolicy policy,Wall::time_point deadline,std::chrono::milliseconds stopBudget)
        :HalDuplexSession(offer.descriptor),buffers_(offer.descriptor,offer.nonce,policy,
            [](uint64_t)noexcept{return mach_absolute_time();}),channel_(std::move(channel)),
         offer_([&]{std::array<uint8_t,NativeDuplexPacket::size> b{};NativeDuplexPacket::encode(offer,b);return b;}()),
         startupDeadline_(deadline),stopBudget_(stopBudget){}
    bool sendUntil(Kind kind,std::span<const uint8_t> bytes,Wall::time_point deadline,bool stopping=false) noexcept {
        while(Wall::now()<deadline && (stopping || !stop_.load(std::memory_order_acquire))) {
            const auto r=channel_->send(kind,bytes);
            if(r==Result::ok)return true;
            if(r!=Result::full)return false;pause();
        }
        return false;
    }
    void run() noexcept {
        scheduling_=AudioThreadScheduling::request();
        Negotiation negotiation(Negotiation::Role::receiver,descriptor.clock.identity,channel_->nonce());
        std::array<uint8_t,NativeDuplexPacket::size> reply{};
        bool active=false,readySent=false,ok=scheduling_.accepted() && negotiation.echo(offer_,mach_absolute_time(),reply) &&
            sendUntil(Kind::start,reply,startupDeadline_);
        AudioFeedBatchFor<6> output;std::array<uint8_t,AudioFeedPacketFor<6>::maxBytes> outputBytes{};
        size_t outputSize=0;bool pending=false;
        uint64_t remoteConsumed=descriptor.outputFirst,sentOutput=descriptor.outputFirst;
        DuplexMachChannel::Packet packet;
        while(ok && !stop_.load(std::memory_order_acquire)) {
            if(!active && (Wall::now()>=startupDeadline_ || !negotiation.poll(mach_absolute_time())))break;
            // Bounded receive work prevents either direction starving the other.
            for(unsigned n=0;n<16 && ok;++n) {
                const auto r=channel_->poll(packet);if(r==Result::idle)break;
                if(r!=Result::ok){ok=false;break;}
                const auto now=mach_absolute_time();
                if(packet.kind==Kind::input)ok=buffers_.receiveInput(packet.payload(),now);
                else if(packet.kind==Kind::clock && active)ok=buffers_.receiveClock(packet.payload(),now);
                else if(packet.kind==Kind::credit && active && packet.used==8) {
                    const auto consumed=DuplexWire::get(packet.payload(),0,8);
                    ok=consumed>=remoteConsumed && consumed<=sentOutput && (consumed-descriptor.outputFirst)%8==0;
                    if(ok)remoteConsumed=consumed;
                }
                else if(packet.kind==Kind::start && !active && !readySent) {
                    NativeDuplexDescriptor accepted;
                    ok=negotiation.receiveCommit(packet.payload(),now,accepted) && accepted==descriptor &&
                        buffers_.prepared(now) && negotiation.ready(now,reply) && sendUntil(Kind::start,reply,startupDeadline_);
                    readySent=ok;
                } else if(packet.kind==Kind::activate && !packet.used && readySent && !active) {
                    ok=negotiation.poll(now) && buffers_.activate(now);active=ok;
                    if(ok)ready_.store(true,std::memory_order_release);
                } else ok=false;
            }
            if(!ok || !buffers_.pump(mach_absolute_time()))break;
            if(active) {
                // Catch up after a delayed wake without relying on 100-us
                // sleeps being honored. Credits still bound all in-flight PCM.
                for(unsigned n=0;n<8 && ok;++n) {
                    if(!pending && buffers_.copyPlaybackBatch(output,8192-(sentOutput-remoteConsumed))) {
                        ok=AudioFeedPacketFor<6>::encode(output,outputBytes,outputSize)==AudioFeedError::ok;pending=ok;
                    }
                    if(!pending || !ok || output.frames>8192-(sentOutput-remoteConsumed))break;
                    const auto r=channel_->send(Kind::output,std::span(outputBytes).first(outputSize));
                    if(r==Result::ok){ok=buffers_.commitPlayback(output.firstFrame,output.frames);sentOutput+=output.frames;pending=false;}
                    else {if(r!=Result::full)ok=false;break;}
                }
            }
            pause();
        }
        // Local callback admission closes before remote confirmation. Stop ACK
        // must mean native users/queues have actually quiesced, not merely that
        // the request was received. Missing/negative confirmation blocks restart.
        diagnostic_=buffers_.ownerDiagnostic();ready_.store(false,std::memory_order_release);buffers_.close();
        const auto deadline=Wall::now()+stopBudget_;
        bool confirmed=false;
        if(channel_->healthy() && sendUntil(Kind::stop,{},deadline,true)) {
            while(Wall::now()<deadline) {
                const auto r=channel_->poll(packet);
                if(r==Result::idle){pause();continue;}
                if(r!=Result::ok)break;
                if(packet.kind==Kind::stopped) {
                    confirmed=packet.used==4 && DuplexWire::get(packet.payload(),0,4)==1;break;
                }
                // Already queued source input/clock/start may precede its ACK.
                if(packet.kind!=Kind::input && packet.kind!=Kind::clock && packet.kind!=Kind::start &&
                   packet.kind!=Kind::activate && packet.kind!=Kind::credit)break;
            }
        }
        localClosed_=channel_->close();cleanupOK_=localClosed_ && confirmed;done_.store(true,std::memory_order_release);
    }
public:
    // Explicit budgets are caller policy, not latency measurements. Rights are
    // consumed on every outcome. A failed remote confirmation remains false even
    // though this owner's thread was joined and its local resources were closed.
    static HalDuplexPreparation prepare(std::unique_ptr<DuplexMachChannel> channel,SourceFrameClock::Identity expected,
                                        SustainedClockPolicy policy,std::chrono::milliseconds startupBudget,
                                        std::chrono::milliseconds stopBudget,const std::atomic<bool> &cancel,bool *failedLocalCleanup=nullptr) {
        if(failedLocalCleanup)*failedLocalCleanup=false;
        if(!channel){if(failedLocalCleanup)*failedLocalCleanup=true;return {nullptr,true};}
        // Bound duration arithmetic before adding to steady_clock::now(). These
        // are control-operation limits, never an audio latency/age policy.
        if(startupBudget.count()<=0 || startupBudget>std::chrono::seconds(60) ||
           stopBudget.count()<=0 || stopBudget>std::chrono::seconds(60)) {
            const bool closed=channel->close();if(failedLocalCleanup)*failedLocalCleanup=closed;return {nullptr,false};
        }
        const auto deadline=Wall::now()+startupBudget;DuplexMachChannel::Packet packet;bool got=false;
        if(channel->healthy() && expected.epoch==channel->epoch() && policy.valid()) {
            while(Wall::now()<deadline && !cancel.load(std::memory_order_acquire)) {
                const auto r=channel->poll(packet);
                if(r==Result::ok){got=true;break;}
                if(r!=Result::idle)break;pause();
            }
        }
        NativeDuplexMessage offer;
        if(!got || packet.kind!=Kind::start || !NativeDuplexPacket::decode(packet.payload(),offer) ||
           offer.kind!=NativeDuplexMessage::Kind::offer || offer.nonce!=channel->nonce() ||
           offer.descriptor.clock.identity!=expected || offer.descriptor.inputChannels!=8 || offer.descriptor.outputChannels!=6 ||
           !offer.descriptor.validAt(mach_absolute_time())) {
            // Peer may have native resources even when no valid offer arrived.
            // Provisioning supervisor must resolve that ownership; never claim
            // remote cleanup just because the local rights close successfully.
            const bool closed=channel->close();if(failedLocalCleanup)*failedLocalCleanup=closed;return {nullptr,false};
        }
        auto session=std::unique_ptr<HalDuplexMachSession>(new HalDuplexMachSession(offer,std::move(channel),policy,deadline,stopBudget));
        if(pthread_create(&session->thread_,nullptr,[](void *p)->void*{static_cast<HalDuplexMachSession *>(p)->run();return nullptr;},session.get())!=0){
            const bool closed=session->channel_->close();if(failedLocalCleanup)*failedLocalCleanup=closed;
            return {nullptr,false};
        }
        session->started_=true;
        while(!session->ready_.load(std::memory_order_acquire) && !session->done_.load(std::memory_order_acquire) &&
              Wall::now()<deadline && !cancel.load(std::memory_order_acquire))pause();
        if(cancel.load(std::memory_order_acquire) || !session->ready(mach_absolute_time())) {
            const bool clean=session->stopAndJoin();if(failedLocalCleanup)*failedLocalCleanup=session->localClosed_;return {nullptr,clean};
        }
        return {std::move(session),true};
    }
    ~HalDuplexMachSession(){stopAndJoin();}
    auto diagnosticAfterJoin() const noexcept {return diagnostic_;} // Only after stopAndJoin().
    auto schedulingAfterJoin()const noexcept{return scheduling_;} // Only after stopAndJoin().
    bool localCleanupConfirmed()const noexcept{return joined_ && localClosed_;} // Only after stopAndJoin().
    bool ready(uint64_t now) const noexcept override {
        // Output may already be copied to the peer. Initial admission still
        // requires retained input and a fresh coherent clock, not untouched TX.
        return ready_.load(std::memory_order_acquire) && buffers_.admissible(now);
    }
    bool ended() const noexcept override {
        return stop_.load(std::memory_order_acquire) || !ready_.load(std::memory_order_acquire);
    }
    void requestStop() const noexcept override {buffers_.requestStop();stop_.store(true,std::memory_order_release);}
    bool stopAndJoin() override {
        requestStop();
        if(started_ && !joined_) {
            // Join failure cannot make freeing a live connection safe.
            if(pthread_join(thread_,nullptr)!=0)std::terminate();joined_=true;
        }
        return cleanupOK_;
    }
    bool timestamp(uint64_t now,SourceFrameClock::Stamp &out) const noexcept override {return buffers_.timestamp(now,out);}
    bool readInput(uint64_t now,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<float> out) const noexcept override {
        return buffers_.readInput(now,cycle,frames,out);
    }
    bool writeOutput(uint64_t now,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<const float> in) const noexcept override {
        return buffers_.writeOutput(now,cycle,frames,in);
    }
};
}
