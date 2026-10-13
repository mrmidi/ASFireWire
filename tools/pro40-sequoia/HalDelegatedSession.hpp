// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuplexHostControl.hpp"
#include "HalDuplexMachSession.hpp"
namespace Pro40Audio {
// Owns only HAL control resources. A separate one-shot completion endpoint
// waits for the controller's joined native/process result after audio STOPPED.
// One thread owns all control IO. Callback-side stop is only atomic signaling.
class HalDelegatedControl final {
    using Clock=std::chrono::steady_clock;
    using Control=DuplexHostControl;
    std::unique_ptr<Control> control_;
    NativeMachPort completion_;
    const audit_token_t controller_;
    const uint64_t nonce_;
    std::thread thread_;
    std::atomic<bool> resolved_{false},committed_{false},wantActive_{false},activeResolved_{false},active_{false},finish_{false};
    std::atomic<uint32_t> finishFlags_{0};
    std::atomic<HalDuplexSession *> inner_{nullptr}; // Retained by preparation/session until this thread joins.
    bool joined_=false,clean_=false,localClosed_=false;
    static void pause(){std::this_thread::sleep_for(std::chrono::microseconds(100));}
    bool send(Control::Kind kind,uint32_t status=0) {
        const auto deadline=Clock::now()+std::chrono::seconds(1);
        do {
            const auto result=control_->send(kind,status);if(result==Control::Result::ok)return true;
            if(result!=Control::Result::full)return false;pause();
        }while(Clock::now()<deadline);
        return false;
    }
    void cancelPeer()noexcept {
        cancelled.store(true);
        if(auto *inner=inner_.load())inner->requestStop();
    }
    void run() {
        const auto deadline=Clock::now()+std::chrono::seconds(2);
        bool ok=send(Control::Kind::ready),committed=false;
        while(ok && !cancelled.load() && Clock::now()<deadline) {
            Control::Event event;const auto result=control_->poll(event);
            if(result==Control::Result::idle){pause();continue;}
            committed=result==Control::Result::ok && event.kind==Control::Kind::committed;break;
        }
        if(!committed)cancelPeer();
        committed_.store(committed);resolved_.store(true);
        while(!finish_.load()) {
            if(wantActive_.load() && !activeResolved_.load()) {
                const bool active=committed && !cancelled.load() && send(Control::Kind::active);
                active_.store(active);activeResolved_.store(true);if(!active)cancelPeer();
            }
            if(ok) {
                Control::Event event;const auto result=control_->poll(event);
                if(result!=Control::Result::idle) {ok=false;cancelPeer();}
                // Only CANCEL is valid after COMMITTED. Any other event, loss
                // or malformed envelope also closes audio admission.
            }
            if(cancelled.load())if(auto *inner=inner_.load())inner->requestStop();
            pause();
        }
        (void)send(Control::Kind::released,finishFlags_.load()|(active_.load()?4:0));
        const bool controlClosed=control_->close();
        const auto cleanupDeadline=Clock::now()+std::chrono::seconds(20);
        bool received=false;
        while(Clock::now()<cleanupDeadline) {
            NativeMachReceived<256> message;
            if(message.poll(completion_.get(),nativeAuditReceiveOptions)) {
                const auto receipt=DuplexHostWire::receipt(message,completion_.get(),controller_,nonce_);
                received=receipt && *receipt;break;
            }
            if(message.status()!=MACH_RCV_TIMED_OUT)break;pause();
        }
        const bool completionClosed=completion_.close()==KERN_SUCCESS;
        localClosed_=controlClosed && completionClosed;clean_=localClosed_ && received;
    }
public:
    std::atomic<bool> cancelled{false};
    HalDelegatedControl(std::unique_ptr<Control> control,NativeMachPort completion,audit_token_t controller,uint64_t nonce)
        :control_(std::move(control)),completion_(std::move(completion)),controller_(controller),nonce_(nonce){}
    ~HalDelegatedControl(){if(thread_.joinable())std::terminate();}
    [[nodiscard]] bool start() {
        if(thread_.joinable() || joined_ || !control_)return false;
        try {thread_=std::thread([this]{run();});}catch(...) {return false;}
        return true;
    }
    [[nodiscard]] bool awaitCommit() {
        while(!resolved_.load())pause();return committed_.load() && !cancelled.load();
    }
    void attach(HalDuplexSession *inner)noexcept {inner_.store(inner);if(cancelled.load() && inner)inner->requestStop();}
    [[nodiscard]] bool activate() {
        wantActive_.store(true);while(!activeResolved_.load())pause();return active_.load() && !cancelled.load();
    }
    void requestStop()noexcept {cancelPeer();}
    // Call after local audio users/thread have joined. Flags describe audio
    // cleanup, not this still-open control/receipt channel.
    [[nodiscard]] bool finishAfterAudio(bool localAudio,bool nativeStopped) {
        if(!joined_) {
            finishFlags_.store((localAudio?1:0)|(nativeStopped?2:0));finish_.store(true);
            if(thread_.joinable())thread_.join();
            else {
                const bool control=control_ && control_->close(),completion=completion_.close()==KERN_SUCCESS;
                localClosed_=control && completion;clean_=false;
            }
            inner_.store(nullptr);joined_=true;
        }
        return clean_;
    }
};

class HalDelegatedSession final:public HalDuplexSession {
    using Clock=std::chrono::steady_clock;
    std::unique_ptr<HalDuplexSession> audio_;
    std::unique_ptr<HalDelegatedControl> control_;
    bool joined_=false,cleanupOK_=false;
    HalDelegatedSession(std::unique_ptr<HalDuplexSession> audio,std::unique_ptr<HalDelegatedControl> control)
        :HalDuplexSession(audio->descriptor),audio_(std::move(audio)),control_(std::move(control)){}
    static bool close(NativeMachPort &a,NativeMachPort &b)noexcept {
        const auto x=a.close(),y=b.close();return x==KERN_SUCCESS && y==KERN_SUCCESS;
    }
    static bool waitReceipt(NativeMachPort &completion,audit_token_t controller,uint64_t nonce) {
        const auto deadline=Clock::now()+std::chrono::seconds(20);bool clean=false;
        while(Clock::now()<deadline) {
            NativeMachReceived<256> message;
            if(message.poll(completion.get(),nativeAuditReceiveOptions)) {
                const auto receipt=DuplexHostWire::receipt(message,completion.get(),controller,nonce);clean=receipt && *receipt;break;
            }
            if(message.status()!=MACH_RCV_TIMED_OUT)break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        return completion.close()==KERN_SUCCESS && clean;
    }
public:
    // Called by the HAL's trusted preparation factory after a code-authenticated
    // broker lookup. No PID discovery or requirements learned from messages.
    static HalDuplexPreparation prepare(std::unique_ptr<DuplexProvisionRegistry::Introduction> controller,DuplexHostWire::Values values,
                                       SustainedClockPolicy policy) {
        if(!controller || !values.valid() || !policy.valid())return {nullptr,true};
        NativeMachPort reply(MACH_PORT_RIGHT_RECEIVE),audio(MACH_PORT_RIGHT_RECEIVE),completion(MACH_PORT_RIGHT_RECEIVE);
        if(reply.allocate()!=KERN_SUCCESS || audio.allocate()!=KERN_SUCCESS || completion.allocate()!=KERN_SUCCESS) {
            const bool ports=close(reply,audio),done=completion.close()==KERN_SUCCESS;return {nullptr,ports && done};
        }
        if(DuplexHostWire::start(controller->controller.get(),reply.get(),audio.get(),completion.get(),values)!=KERN_SUCCESS) {
            const bool ports=close(reply,audio),done=completion.close()==KERN_SUCCESS;return {nullptr,ports && done};
        }
        std::unique_ptr<DuplexHostWire::Offered> offered;
        const auto deadline=Clock::now()+std::chrono::seconds(35);
        while(Clock::now()<deadline) {
            NativeMachReceived<512> message;
            if(message.poll(reply.get(),nativeAuditReceiveOptions)) {
                offered=DuplexHostWire::receiveOffer(message,reply.get(),controller->token,values);break;
            }
            if(message.status()!=MACH_RCV_TIMED_OUT)break;
            // A denied release may complete without publishing a worker offer.
            NativeMachReceived<256> ended;
            if(ended.poll(completion.get(),nativeAuditReceiveOptions)) {
                const auto receipt=DuplexHostWire::receipt(ended,completion.get(),controller->token,values.nonce);
                const bool ports=close(reply,audio),done=completion.close()==KERN_SUCCESS;
                return {nullptr,ports && done && receipt && *receipt};
            }
            if(ended.status()!=MACH_RCV_TIMED_OUT)break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        if(!offered) {
            const bool ports=close(reply,audio),done=waitReceipt(completion,controller->token,values.nonce);return {nullptr,ports && done};
        }
        auto channel=DuplexRendezvousWire::channel(audio,offered->worker,offered->token,offered->binding);
        auto controlChannel=std::make_unique<DuplexHostControl>(std::move(reply),std::move(offered->control),controller->token,values.nonce);
        auto control=std::make_unique<HalDelegatedControl>(std::move(controlChannel),std::move(completion),controller->token,values.nonce);
        if(!control->start() || !control->awaitCommit()) {
            const bool local=channel->close(),done=control->finishAfterAudio(local,false);return {nullptr,local && done};
        }
        bool failedLocal=false;
        auto prepared=HalDuplexMachSession::prepare(std::move(channel),offered->identity,policy,
            std::chrono::seconds(15),std::chrono::seconds(2),control->cancelled,&failedLocal);
        if(!prepared.session) {
            const bool done=control->finishAfterAudio(failedLocal,prepared.cleanupOK);return {nullptr,failedLocal && prepared.cleanupOK && done};
        }
        control->attach(prepared.session.get());
        if(!control->activate()) {
            const bool peer=prepared.session->stopAndJoin();
            const bool local=static_cast<HalDuplexMachSession &>(*prepared.session).localCleanupConfirmed();
            const bool done=control->finishAfterAudio(local,peer);return {nullptr,local && peer && done};
        }
        return {std::unique_ptr<HalDuplexSession>(new HalDelegatedSession(std::move(prepared.session),std::move(control))),true};
    }
    ~HalDelegatedSession(){(void)stopAndJoin();}
    bool ready(uint64_t now)const noexcept override{return !control_->cancelled.load() && audio_->ready(now);}
    bool ended()const noexcept override{return control_->cancelled.load() || audio_->ended();}
    void requestStop()const noexcept override {control_->requestStop();audio_->requestStop();}
    bool stopAndJoin()override {
        if(!joined_) {
            requestStop();const bool peer=audio_->stopAndJoin();
            const bool local=static_cast<HalDuplexMachSession &>(*audio_).localCleanupConfirmed();
            const bool controller=control_->finishAfterAudio(local,peer);
            cleanupOK_=peer && local && controller;joined_=true;
        }
        return cleanupOK_;
    }
    bool timestamp(uint64_t now,SourceFrameClock::Stamp &out)const noexcept override {return !control_->cancelled.load() && audio_->timestamp(now,out);}
    bool readInput(uint64_t now,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<float> out)const noexcept override {
        return !control_->cancelled.load() && audio_->readInput(now,cycle,frames,out);
    }
    bool writeOutput(uint64_t now,const AudioServerPlugInIOCycleInfo &cycle,UInt32 frames,std::span<const float> in)const noexcept override {
        return !control_->cancelled.load() && audio_->writeOutput(now,cycle,frames,in);
    }
};
}
