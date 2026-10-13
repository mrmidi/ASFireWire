// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "WorkerStartupOwner.hpp"
#include "../NativeOriginalPeerCapture.hpp"
namespace Pro40Lab {
struct WorkerCaptureReport {
    enum class Status {waiting,captured,stopped,timeout,invalid,io,scheduling,unavailable};
    Status status=Status::waiting;
    bool endpointsClosed=false;
    kern_return_t error=KERN_SUCCESS;
    std::optional<Pro40Audio::NativeOriginalPeerCapture> capture;
};
// Serial-queue, one-shot introduction capture. No identity authorization here.
// Prepare before spawn, borrow bootstrap() through startup, then start polling.
class WorkerCaptureOwner final : public std::enable_shared_from_this<WorkerCaptureOwner> {
    using Clock=std::chrono::steady_clock;
    dispatch_queue_t queue_;
    dispatch_source_t timer_=nullptr;
    Pro40Audio::NativeMachPort receive_{MACH_PORT_RIGHT_RECEIVE},send_{MACH_PORT_RIGHT_SEND};
    std::shared_ptr<WorkerStartupOwner> startup_;
    std::function<void(WorkerCaptureReport)> report_;
    Clock::time_point deadline_;
    WorkerCaptureReport state_;
    bool prepared_=false,started_=false,finished_=false;
#ifdef PRO40_CAPTURE_TESTING
    bool failTimer_=false;
#endif
    WorkerCaptureOwner(dispatch_queue_t queue,std::function<void(WorkerCaptureReport)> report):queue_(queue),report_(std::move(report)){}
    void finish(WorkerCaptureReport::Status status,kern_return_t error=KERN_SUCCESS){
        if(finished_)return;finished_=true;
        state_.status=status;state_.error=error;
        const auto sent=send_.close(),received=receive_.close();
        state_.endpointsClosed=sent==KERN_SUCCESS && received==KERN_SUCCESS;
        if(!state_.endpointsClosed){state_.status=WorkerCaptureReport::Status::io;state_.error=sent?sent:received;}
        if(state_.status!=WorkerCaptureReport::Status::captured){state_.capture.reset();if(startup_)startup_->requestStop();}
        // Retain an unresolved port owner; a failed close never becomes clean.
        if(timer_ && state_.endpointsClosed){dispatch_source_cancel(timer_);timer_=nullptr;}
        auto report=std::move(report_);if(report)report(state_);
    }
    void tick(){
        dispatch_assert_queue(queue_);if(finished_)return;
        using Status=WorkerCaptureReport::Status;
        if(startup_->stopQuery().requested()){finish(Status::stopped);return;}
        if(Clock::now()>=deadline_){finish(Status::timeout);return;}
        if(!startup_->readyToVerify()){finish(Status::unavailable);return;}
        std::optional<Pro40Audio::NativeOriginalPeerCapture> captured;
        kern_return_t error=KERN_SUCCESS;bool received=false;
        {Pro40Audio::NativeMachReceived<> message;
            received=message.poll(receive_.get(),Pro40Audio::nativeAuditReceiveOptions);error=message.status();
            if(received)captured=Pro40Audio::NativeOriginalPeerCapture::capture(message,receive_.get(),startup_->retainedChild()->child().pid());
        } // Received rights destroyed before closing the capture endpoint.
        if(startup_->stopQuery().requested()){finish(Status::stopped);return;}
        if(Clock::now()>=deadline_){finish(Status::timeout);return;}
        if(received){state_.capture=captured;finish(captured?Status::captured:Status::invalid);}
        else if(error!=MACH_RCV_TIMED_OUT)finish(Status::io,error);
    }
public:
    static std::shared_ptr<WorkerCaptureOwner> create(dispatch_queue_t queue,std::function<void(WorkerCaptureReport)> report){
        if(!queue || !report)return {};
        return std::shared_ptr<WorkerCaptureOwner>(new WorkerCaptureOwner(queue,std::move(report)));
    }
    [[nodiscard]] bool prepare(){
        dispatch_assert_queue(queue_);if(prepared_ || finished_)return false;prepared_=true;
        auto error=receive_.allocate();
        if(!error)error=mach_port_insert_right(mach_task_self(),receive_.get(),receive_.get(),MACH_MSG_TYPE_MAKE_SEND);
        if(!error)error=send_.adopt(receive_.get());
        if(error){finish(WorkerCaptureReport::Status::io,error);return false;}return true;
    }
    mach_port_t bootstrap()const{dispatch_assert_queue(queue_);return finished_?MACH_PORT_NULL:receive_.get();}
#ifdef PRO40_CAPTURE_TESTING
    void failTimerForTesting(){dispatch_assert_queue(queue_);failTimer_=true;}
#endif
    [[nodiscard]] bool start(std::shared_ptr<WorkerStartupOwner> startup,Clock::time_point deadline){
        dispatch_assert_queue(queue_);if(started_ || !prepared_ || finished_)return false;started_=true;
        startup_=std::move(startup);deadline_=deadline;
        if(!startup_ || !startup_->readyToVerify()){finish(WorkerCaptureReport::Status::unavailable);return false;}
#ifdef PRO40_CAPTURE_TESTING
        if(!failTimer_)
#endif
        timer_=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,queue_);
        if(!timer_){finish(WorkerCaptureReport::Status::scheduling);return false;}
        auto retained=shared_from_this();dispatch_source_set_event_handler(timer_,^{retained->tick();});
        dispatch_source_set_timer(timer_,dispatch_time(DISPATCH_TIME_NOW,0),5*NSEC_PER_MSEC,NSEC_PER_MSEC);
        dispatch_resume(timer_);return true;
    }
    void cancel(){dispatch_assert_queue(queue_);finish(WorkerCaptureReport::Status::stopped);}
    WorkerCaptureReport snapshot()const{dispatch_assert_queue(queue_);return state_;}
};
}
