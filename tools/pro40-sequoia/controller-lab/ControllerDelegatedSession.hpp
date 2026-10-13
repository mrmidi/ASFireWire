// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "WorkerDuplexConnectionOwner.hpp"
#include "../DuplexHostControl.hpp"
#include "../worker/ManagedRunTiming.hpp"
namespace Pro40Lab {
// Retained by the controller command. run() executes on its joined delegation
// thread; finishAfterJoin() executes only after the command has joined that
// thread AND its child/verifier/watchdog owners. No concurrent method calls.
// The final receipt endpoint deliberately outlives per-session control ports.
class ControllerDelegatedSession final {
    using Clock=std::chrono::steady_clock;
    using Control=Pro40Audio::DuplexHostControl;
    std::unique_ptr<Pro40Audio::DuplexHostWire::Request> request_;
    const Pro40Audio::SourceFrameClock::Identity identity_;
    const std::optional<std::chrono::milliseconds> runBudget_;
    std::unique_ptr<Control> control_;
    bool attempted_=false,finished_=false,controlClosed_=true;
    static void pause(){std::this_thread::sleep_for(std::chrono::microseconds(100));}
public:
    static constexpr bool validRunBudget(std::optional<std::chrono::milliseconds> budget) {
        return !budget || (budget->count()>0 && *budget<=Pro40::ManagedRunTiming::run);
    }
    ControllerDelegatedSession(std::unique_ptr<Pro40Audio::DuplexHostWire::Request> request,
        Pro40Audio::SourceFrameClock::Identity identity,std::optional<std::chrono::milliseconds> budget)
        :request_(std::move(request)),identity_(identity),runBudget_(budget){}
    WorkerDuplexReport run(std::unique_ptr<Pro40Audio::DuplexEndpointGrant> grant,audit_token_t worker,
                          Pro40Audio::DuplexLaunchBinding binding,const std::atomic<bool> &cancel) {
        using namespace Pro40Audio;WorkerDuplexReport report;
        if(attempted_ || finished_ || !request_ || !grant || !validRunBudget(runBudget_)) {
            attempted_=true;
            // No offer/commit occurred. Retire every supplied capability before
            // reporting the rejected attempt; a default localClosed=false would
            // otherwise leave command completion waiting after the thread joins.
            const bool grantClosed=!grant || grant->close();
            const bool replyClosed=!request_ || request_->reply.close()==KERN_SUCCESS;
            const bool audioClosed=!request_ || request_->audio.close()==KERN_SUCCESS;
            report.localClosed=grantClosed && replyClosed && audioClosed && controlClosed_;
            report.finished=true;return report;
        }
        attempted_=true;
        NativeMachPort receive(MACH_PORT_RIGHT_RECEIVE);
        bool ready=false,committed=false,released=false,cancelSent=false,protocolOK=true;
        std::optional<Clock::time_point> deadline=Clock::now()+std::chrono::seconds(1);
        if(receive.allocate()==KERN_SUCCESS && DuplexHostWire::offer(*request_,grant->workerPort(),receive.get(),worker,binding,identity_)==KERN_SUCCESS) {
            control_=std::make_unique<Control>(std::move(receive),std::move(request_->reply),request_->helper,request_->values.nonce);
            controlClosed_=false;
            while(Clock::now()<*deadline && !cancel.load()) {
                Control::Event event;const auto result=control_->poll(event);
                if(result==Control::Result::idle){pause();continue;}
                ready=result==Control::Result::ok && event.kind==Control::Kind::ready;break;
            }
            committed=ready && grant->commit(request_->audio.get(),request_->helper,cancel.load());
            if(committed)committed=control_->send(Control::Kind::committed)==Control::Result::ok;
            if(!committed){(void)control_->send(Control::Kind::cancel);cancelSent=true;deadline=Clock::now()+std::chrono::seconds(5);}
            else deadline=runBudget_?std::optional(Clock::now()+*runBudget_):std::nullopt;
            for(;;) {
                const auto now=Clock::now();
                if(!cancelSent && (cancel.load() || (deadline && now>=*deadline))) {
                    // Run expiry is cancellation, not successful completion.
                    // Keep the original endpoint for the same bounded RELEASED
                    // grace as explicit cancellation; never close it at CANCEL.
                    (void)control_->send(Control::Kind::cancel);cancelSent=true;deadline=now+std::chrono::seconds(5);
                }
                if(deadline && now>=*deadline)break;
                Control::Event event;const auto result=control_->poll(event);
                if(result==Control::Result::idle){pause();continue;}
                if(result!=Control::Result::ok){protocolOK=false;break;}
                // ACTIVE may already be in flight when local cancellation
                // crosses it. Record that committed admission, then still wait
                // for RELEASED; cancellation must not discard cleanup evidence.
                if(event.kind==Control::Kind::active && committed && !report.admitted)report.admitted=true;
                else if(event.kind==Control::Kind::released && bool(event.status&4)==report.admitted) {
                    report.peerClean=(event.status&3)==3;released=true;break;
                }else {protocolOK=false;break;}
            }
            if(!released && !cancelSent){(void)control_->send(Control::Kind::cancel);cancelSent=true;}
            controlClosed_=control_->close();control_.reset();
        }
        // finishAfterJoin also closes these for a denied worker release, where
        // this function is never entered. Neither closure claims remote cleanup.
        const bool grantClosed=grant->close(),replyClosed=request_->reply.close()==KERN_SUCCESS;
        const bool audioClosed=request_->audio.close()==KERN_SUCCESS,receiveClosed=receive.close()==KERN_SUCCESS;
        report.localClosed=grantClosed && replyClosed && audioClosed && receiveClosed && controlClosed_;
        report.cancelled=cancel.load() || cancelSent;
        report.streamPassed=protocolOK && committed && report.admitted && released && report.peerClean && !report.cancelled;
        report.finished=true;return report;
    }
    // nativeOwnersClean must be the final retained command's cleanup result,
    // including native outcome when it released a worker. Never a HAL assertion.
    [[nodiscard]] bool finishAfterJoin(bool nativeOwnersClean)noexcept {
        if(finished_ || !request_ || control_)return false;finished_=true;
        const bool reply=request_->reply.close()==KERN_SUCCESS,audio=request_->audio.close()==KERN_SUCCESS;
        const bool clean=nativeOwnersClean && reply && audio && controlClosed_;
        const auto sent=Pro40Audio::DuplexHostWire::joined(request_->completion.get(),request_->values.nonce,clean);
        const bool closed=request_->completion.close()==KERN_SUCCESS;
        return sent==KERN_SUCCESS && closed;
    }
};
}
