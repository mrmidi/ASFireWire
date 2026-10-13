// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeDuplexSourceConnection.hpp"
#include "SustainedInputReceiver.hpp"
#include "PlaybackStartupGate.hpp"
#include "NativeSessionRunner.hpp"
#include "AudioThreadScheduling.hpp"
#include "NativeDuplexPolicy.hpp"
#include "NativePlaybackTimeline.hpp"
#include <chrono>
#include <exception>
#include <pthread.h>
#include <thread>
#include <mach/mach_time.h>
namespace Pro40Audio {
struct NativeDuplexBridgePolicy {
    SourceFrameClock::Identity identity;
    SustainedClockPolicy clock;
    uint64_t offerTicks=0,leadFrames=0,primeFrames=0;
    std::chrono::milliseconds qualificationBudget{0};
    bool valid()const noexcept {
        return identity.epoch && identity.seed && identity.numer && identity.denom && clock.valid() && offerTicks &&
            leadFrames && leadFrames%8==0 && leadFrames<=48000 && primeFrames && primeFrames%8==0 &&
            primeFrames<=8192 && leadFrames>=primeFrames && qualificationBudget.count()>0 &&
            qualificationBudget<=std::chrono::seconds(30);
    }
};
enum class NativeDuplexBridgeFault:uint32_t {none,configuration,startup,input,clock,playback,connection};
// Owns the connection/receiver thread, queue and content-only startup gate. All
// storage is constructed before native entry. Only handoff and playback.port()
// cross to the native owner. No native function is called on the bridge thread.
// finish() requires actual native teardown, and joins BEFORE any owner is freed.
class NativeDuplexBridge final {
    using Wall=std::chrono::steady_clock;
    using Receiver=SustainedInputReceiver<2048,nativeDuplexInputWindowFrames>;
    using Connection=NativeDuplexSourceConnection<Receiver>;
    NativeDuplexBridgePolicy policy_;
    Pro40::StopQuery external_;
    std::unique_ptr<DuplexMachChannel> channel_;
    std::unique_ptr<Connection> connection_;
    pthread_t thread_{};
    bool started_=false,joined_=false,nativeClean_=false,localClean_=true;
    std::atomic<bool> stop_{false},nativeDone_{false};
    std::atomic<NativeDuplexBridgeFault> fault_{NativeDuplexBridgeFault::none};
    bool armed_=false,confirmed_=false; // Bridge owner, inspect after join only.
    uint64_t offeredFirst_=UINT64_MAX;
    SourceFrameClock offeredClock_{};
    ControllerClockFit::Position offeredController_{};
    NativeClockSnapshot offeredSnapshot_{};
    NativePlaybackTimeline offeredPresentation_{};
    AudioThreadScheduling scheduling_{}; // Read only after finish joins.
    struct StartupObservation {
        NativeClockSnapshot snapshot{};
        uint64_t now=0,prefix=0,oldest=0,next=0;
        unsigned rejection=0; // 1 identity, 2 age, 3 quality, 4 history/range, 5 RX conversion, 6 TX mapping.
    } startupObservation_; // Bridge owner, inspect after join only.
    void fail(NativeDuplexBridgeFault why)noexcept {
        auto none=NativeDuplexBridgeFault::none;fault_.compare_exchange_strong(none,why,std::memory_order_acq_rel);
        stop_.store(true,std::memory_order_release);
    }
    void offer(uint64_t now) {
        NativeClockSnapshot s;if(!receiver.snapshot(s) || !policy_.clock.enough(s,receiver.snapshotFrames()))return;
        const auto &id=policy_.identity;
        startupObservation_.snapshot=s;startupObservation_.now=now;startupObservation_.prefix=receiver.snapshotFrames();
        const auto reject=[&](unsigned why){startupObservation_.rejection=why;fail(NativeDuplexBridgeFault::clock);};
        if(s.generation!=id.generation || s.numer!=id.numer || s.denom!=id.denom){reject(1);return;}
        if(s.controller.lastHost>now || now-s.controller.lastHost>policy_.clock.maxArrivalAge){reject(2);return;}
        if(!policy_.clock.acceptsSnapshot(s)){reject(3);return;}
        NativeDuplexDescriptor d;uint64_t prefix=0;
        if(!nativePastClockProposal(s,id,receiver.snapshotFrames(),policy_.clock.lookAheadFrames,d.clock,prefix)){reject(5);return;}
        if(!policy_.clock.enough(s,prefix))return;
        startupObservation_.prefix=prefix;
        auto input=receiver.tryAcquire();
        if(!input || input->next()<prefix)return;
        startupObservation_.oldest=input->oldest();startupObservation_.next=input->next();
        // Keep the independently tested callback history from the shared policy.
        // The HAL-side buffer now publishes new PCM between clock reports;
        // retain scheduling slack and the full8192-frame storage window.
        constexpr uint64_t inputHistoryFrames=nativeDuplexInputHistoryFrames;
        if(prefix<inputHistoryFrames || input->oldest()>prefix-inputHistoryFrames || now>UINT64_MAX-policy_.offerTicks ||
           prefix>SourceFrameClock::maxExactFrame-policy_.clock.lookAheadFrames) {reject(4);return;}
        const auto next=playback.nextNativeFrame();
        if(next>SourceFrameClock::maxExactFrame-policy_.leadFrames) {fail(NativeDuplexBridgeFault::playback);return;}
        const auto first=next+policy_.leadFrames;
        Pro40Packets::PlaybackPacketPosition initialTx;
        if(!playback.firstPosition(initialTx))return; // No native timing guess before first content packet.
        NativePlaybackTimeline presentation;
        if(!nativePlaybackTimeline(initialTx,s,d.clock,first,presentation)){reject(6);return;}
        d.evidenceSequence=receiver.clockPairs();d.prefixFrames=prefix;d.evidenceHost=s.controller.lastHost;
        d.preparedHost=now;d.expiresHost=now+policy_.offerTicks;d.inputFirst=prefix-inputHistoryFrames;d.initialInputFrames=512;
        d.outputFirst=0;d.primedOutputFrames=policy_.primeFrames;
        d.inputTimeline={id.epoch,id.seed,0,0};
        // Queue frame0 corresponds to this session's scheduled presentation
        // on the shared RX clock. ADC/DAC latency remains separately uncalibrated.
        d.outputTimeline={id.epoch,id.seed,0,int64_t(presentation.sampleAtCutover)};
        if(!d.validAt(now) || !playback.schedule(first,policy_.primeFrames)) {fail(NativeDuplexBridgeFault::playback);return;}
        offeredFirst_=first;
        offeredClock_=d.clock;
        offeredController_=s.controller;offeredSnapshot_=s;
        offeredPresentation_=presentation;
        connection_=std::make_unique<Connection>(std::move(channel_),receiver,output,d,policy_.clock,now);
        if(connection_->failed())fail(NativeDuplexBridgeFault::connection);
    }
    void consume()noexcept {
        scheduling_=AudioThreadScheduling::request();
        if(!scheduling_.accepted()){fail(NativeDuplexBridgeFault::startup);return;}
        const auto deadline=Wall::now()+policy_.qualificationBudget;
        std::optional<Wall::time_point> closeDeadline;
        for(;;) {
            const bool done=nativeDone_.load(std::memory_order_acquire);
            if(done && !closeDeadline)closeDeadline=Wall::now()+std::chrono::milliseconds(500);
            if(external_.requested())stop_.store(true,std::memory_order_release);
            if(playback.fault()!=PlaybackStartupFault::none)fail(NativeDuplexBridgeFault::playback);
            if(!done && !stop_.load(std::memory_order_acquire)) {
                if(!receiver.poll())fail(NativeDuplexBridgeFault::input);
                if(!connection_ && !stop_.load(std::memory_order_acquire)) {
                    if(Wall::now()>=deadline)fail(NativeDuplexBridgeFault::startup);
                    else try{offer(mach_absolute_time());}catch(...){fail(NativeDuplexBridgeFault::startup);}
                }
            }
            if(connection_) {
                if(done && !connection_->stopRequested())connection_->requestStop();
                (void)connection_->poll(mach_absolute_time());
                if(connection_->failed())fail(NativeDuplexBridgeFault::connection);
                if(connection_->stopRequested())stop_.store(true,std::memory_order_release);
                if(!done && !armed_ && connection_->primed()) {
                    if(!playback.arm())fail(NativeDuplexBridgeFault::playback);else armed_=true;
                }
                if(done && connection_->stopRequested() && !confirmed_) {
                    confirmed_=connection_->confirmStopped(nativeClean_);
                }
                if(done && (connection_->closed() || Wall::now()>=*closeDeadline))break;
            } else if(done)break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
public:
    SustainedInputHandoff handoff;
    Receiver receiver;
    PlaybackPcmQueue<> output;
    PlaybackStartupGate playback;
    Pro40Packets::PlaybackPcmSource source;
    NativeDuplexBridge(std::unique_ptr<DuplexMachChannel> channel,NativeDuplexBridgePolicy policy,Pro40::StopQuery external={})
        :policy_(policy),external_(external),channel_(std::move(channel)),handoff(policy.identity.numer,policy.identity.denom),
         receiver(handoff,policy.identity.epoch),output(policy.identity.epoch),playback(output),source(playback.port()) {
        if(!policy.valid() || !channel_ || !channel_->healthy() || channel_->epoch()!=policy.identity.epoch)fail(NativeDuplexBridgeFault::configuration);
    }
    ~NativeDuplexBridge(){if(started_ && !joined_)std::terminate();} // Never silently free a live native view.
    bool start()noexcept {
        if(started_ || fault()!=NativeDuplexBridgeFault::none)return false;
        if(pthread_create(&thread_,nullptr,[](void *p)->void*{static_cast<NativeDuplexBridge *>(p)->consume();return nullptr;},this)) {
            fail(NativeDuplexBridgeFault::startup);return false;
        }
        started_=true;return true;
    }
    bool requested()const noexcept{return stop_.load(std::memory_order_acquire);}
    NativeDuplexBridgeFault fault()const noexcept{return fault_.load(std::memory_order_acquire);}
    bool finish(bool nativeClean)noexcept { // Main owner, strictly AFTER native teardown.
        if(!started_)return true;
        if(!joined_) {
            nativeClean_=nativeClean;nativeDone_.store(true,std::memory_order_release);
            if(pthread_join(thread_,nullptr)!=0)std::terminate();joined_=true;
            // Connection and its private rights have no concurrent users now.
            if(connection_)localClean_=connection_->closeLocal();
            if(channel_)localClean_=channel_->close() && localClean_;
            connection_.reset();channel_.reset();
        }
        return nativeClean_ && localClean_;
    }
    uint64_t offeredFirst()const noexcept{return offeredFirst_;} // After finish only.
    const auto &offeredClock()const noexcept{return offeredClock_;} // After finish only.
    const auto &startupObservation()const noexcept{return startupObservation_;} // After finish only.
    const auto &offeredSnapshot()const noexcept{return offeredSnapshot_;} // After finish only.
    const auto &offeredController()const noexcept{return offeredController_;} // After finish only.
    const auto &offeredPresentation()const noexcept{return offeredPresentation_;} // After finish only.
    const auto &schedulingAfterJoin()const noexcept{return scheduling_;}
};
}
