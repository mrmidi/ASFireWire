// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../DuplexMachRendezvous.hpp"
#include "../HalDuplexMachSession.hpp"
#include <dispatch/dispatch.h>
#include <thread>
namespace Pro40Lab {
struct WorkerDuplexReport {
    bool finished=false,localClosed=false,admitted=false,streamPassed=false,peerClean=false,cancelled=false;
    Pro40Audio::DuplexSessionBuffers<>::Diagnostic diagnostic{};
    Pro40Audio::AudioThreadScheduling renderScheduling{},connectionScheduling{};
};
// Controller-owned audio capability, separate from child/result ownership.
// start() is called ONLY after VerifierStartupEntry releases the exact retained
// child. It does not establish code identity itself. Its private receive right
// comes from a declared Mach service; the ordinary bootstrap remains inherited.
// No SDK registration or blocking audio work occurs on the controller queue.
class WorkerDuplexConnectionOwner final:public std::enable_shared_from_this<WorkerDuplexConnectionOwner> {
public:
    using Session=Pro40Audio::HalDuplexMachSession;
    using Render=std::function<bool(Session &,const std::atomic<bool> &)>;
    // The controller still retains the exact worker and joins this owner. A
    // delegation function must complete its private handoff/control work before
    // returning; it never transfers child/verifier ownership to the HAL.
    using Delegate=std::function<WorkerDuplexReport(std::unique_ptr<Pro40Audio::DuplexEndpointGrant>,
        audit_token_t,Pro40Audio::DuplexLaunchBinding,const std::atomic<bool> &)>;
private:
    dispatch_queue_t queue_;
    Pro40Audio::NativeMachPort listener_;
    Pro40Audio::SourceFrameClock::Identity identity_;
    Pro40Audio::SustainedClockPolicy policy_;
    Render render_;
    Delegate delegate_;
    std::thread thread_;
    std::atomic<bool> cancel_{false};
    bool attempted_=false,joined_=false;
    WorkerDuplexReport result_;
    void run(audit_token_t token,Pro40Audio::DuplexLaunchBinding binding){
        using namespace Pro40Audio;using Clock=std::chrono::steady_clock;
        result_.renderScheduling=AudioThreadScheduling::request();
        if(!result_.renderScheduling.accepted()) {
            result_.localClosed=listener_.close()==KERN_SUCCESS;result_.finished=true;return;
        }
        const auto deadline=Clock::now()+std::chrono::seconds(2);
        std::unique_ptr<DuplexMachChannel> channel;std::unique_ptr<DuplexEndpointGrant> grant;DuplexMachAcceptor acceptor;
        while(!cancel_.load() && Clock::now()<deadline) {
            NativeMachReceived<512> message;
            if(message.poll(listener_.get(),nativeAuditReceiveOptions)) {
                if(delegate_)grant=acceptor.prepare(message,listener_.get(),token,binding,deadline);
                else channel=acceptor.accept(message,listener_.get(),token,binding,deadline);
                break;
            }
            if(message.status()!=MACH_RCV_TIMED_OUT)break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        result_.localClosed=listener_.close()==KERN_SUCCESS;
        if(delegate_) {
            if(grant && !cancel_.load()) {
                const bool listenerClosed=result_.localClosed;
                const auto scheduling=result_.renderScheduling;
                result_=delegate_(std::move(grant),token,binding,cancel_);
                result_.renderScheduling=scheduling;
                result_.localClosed=result_.localClosed && listenerClosed;
            }
            result_.cancelled=result_.cancelled || cancel_.load();result_.finished=true;return;
        }
        if(channel && !cancel_.load()) {
            bool failedLocalCleanup=false;
            auto prepared=Session::prepare(std::move(channel),identity_,policy_,std::chrono::seconds(15),std::chrono::seconds(2),cancel_,&failedLocalCleanup);
            result_.peerClean=prepared.cleanupOK;
            if(prepared.session) {
                result_.admitted=true;
                auto &session=static_cast<Session &>(*prepared.session);
                try{result_.streamPassed=render_(session,cancel_);}catch(...){result_.streamPassed=false;}
                // A retained-child completion can arrive while stopAndJoin()
                // waits for STOPPED. That is cleanup, not render cancellation.
                result_.cancelled=cancel_.load();
                result_.peerClean=prepared.session->stopAndJoin();
                result_.diagnostic=session.diagnosticAfterJoin();
                result_.connectionScheduling=session.schedulingAfterJoin();
                result_.localClosed=session.localCleanupConfirmed() && result_.localClosed;
            }else {result_.cancelled=cancel_.load();result_.localClosed=failedLocalCleanup && result_.localClosed;}
        } else if(channel)result_.localClosed=channel->close() && result_.localClosed;
        if(!result_.admitted)result_.cancelled=cancel_.load();
        result_.finished=true;
    }
public:
    WorkerDuplexConnectionOwner(dispatch_queue_t queue,Pro40Audio::NativeMachPort listener,
        Pro40Audio::SourceFrameClock::Identity identity,Pro40Audio::SustainedClockPolicy policy,Render render):
        queue_(queue),listener_(std::move(listener)),identity_(identity),policy_(policy),render_(std::move(render)){}
    WorkerDuplexConnectionOwner(dispatch_queue_t queue,Pro40Audio::NativeMachPort listener,
        Pro40Audio::SourceFrameClock::Identity identity,Pro40Audio::SustainedClockPolicy policy,Delegate delegate):
        queue_(queue),listener_(std::move(listener)),identity_(identity),policy_(policy),delegate_(std::move(delegate)){}
    ~WorkerDuplexConnectionOwner(){if(thread_.joinable())std::terminate();}
    [[nodiscard]] bool start(const audit_token_t &token,Pro40Audio::DuplexLaunchBinding binding,std::function<void()> completed){
        dispatch_assert_queue(queue_);
        if(attempted_ || !queue_ || !MACH_PORT_VALID(listener_.get()) || !binding.valid() || binding.epoch!=identity_.epoch || !policy_.valid() ||
           bool(render_)==bool(delegate_) || !completed)return false;
        attempted_=true;auto retained=shared_from_this();
        try{thread_=std::thread([retained,token,binding,completed]{
            try{retained->run(token,binding);}catch(...){
                (void)retained->listener_.close();retained->result_.localClosed=false;
                retained->result_.finished=true;
            }
            // The queued strong owner survives the worker thread's last ref.
            // Join before exposing results or allowing destruction/restart.
            dispatch_async(retained->queue_,^{retained->thread_.join();retained->joined_=true;completed();});
        });}catch(...){result_.localClosed=listener_.close()==KERN_SUCCESS;result_.finished=true;joined_=true;return false;}
        return true;
    }
    // Denied release: no thread or peer endpoint was ever published.
    void retireUnreleased(){dispatch_assert_queue(queue_);if(attempted_)return;attempted_=true;result_.localClosed=listener_.close()==KERN_SUCCESS;result_.finished=true;joined_=true;}
    void requestStop()noexcept{cancel_.store(true);}
    WorkerDuplexReport snapshot()const{dispatch_assert_queue(queue_);return joined_?result_:WorkerDuplexReport{};}
};
}
