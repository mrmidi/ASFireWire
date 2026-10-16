// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "WorkerStartupOwner.hpp"
#include "../worker/SocketPeerCapture.hpp"
#include <sys/stat.h>
namespace Pro40Lab {
struct WorkerSocketCaptureReport {
    enum class Status {waiting,captured,stopped,timeout,invalid,io,scheduling,unavailable};
    Status status=Status::waiting;
    int error=0;
    bool endpointsClosed=false;
    std::optional<Pro40::SocketPeerCapture> capture;
};
// Owner-queue listener. One accept per tick, no blocking connect/receive or peer
// payload. A private directory owns the socket name; cleanup never retries fd close.
class WorkerSocketCaptureOwner final:public std::enable_shared_from_this<WorkerSocketCaptureOwner> {
    dispatch_queue_t queue_;
    dispatch_source_t timer_=nullptr;
    int listener_=-1;
    int endpointError_=0;
    std::string directory_,path_;
    bool bound_=false,prepared_=false,started_=false,finished_=false;
    std::shared_ptr<WorkerStartupOwner> startup_;
    std::function<void(WorkerSocketCaptureReport)> report_;
    std::chrono::steady_clock::time_point deadline_;
    WorkerSocketCaptureReport state_;
#ifdef PRO40_CAPTURE_TESTING
    bool failTimer_=false;
#endif
    WorkerSocketCaptureOwner(dispatch_queue_t q,std::function<void(WorkerSocketCaptureReport)> report):queue_(q),report_(std::move(report)){}
    int cleanup(){
        auto failed=[&]{if(!endpointError_)endpointError_=errno;};
        if(listener_>=0){int fd=listener_;listener_=-1;if(close(fd))failed();}
        if(bound_){bound_=false;if(unlink(path_.c_str()))failed();}
        if(!directory_.empty()){
            auto directory=std::move(directory_);directory_.clear();
            if(rmdir(directory.c_str()))failed();
        }
        return endpointError_;
    }
    void finish(WorkerSocketCaptureReport::Status status,int error=0){
        if(finished_)return;finished_=true;state_.status=status;state_.error=error;
        const int cleanupError=cleanup();
        state_.endpointsClosed=!cleanupError;
        if(cleanupError){state_.status=WorkerSocketCaptureReport::Status::io;state_.error=cleanupError;}
        if(state_.status!=WorkerSocketCaptureReport::Status::captured){state_.capture.reset();if(startup_)startup_->requestStop();}
        if(timer_ && state_.endpointsClosed){dispatch_source_cancel(timer_);timer_=nullptr;}
        auto report=std::move(report_);if(report)report(state_);
    }
    void tick(){
        dispatch_assert_queue(queue_);if(finished_)return;
        using Status=WorkerSocketCaptureReport::Status;
        if(startup_->stopQuery().requested()){finish(Status::stopped);return;}
        if(std::chrono::steady_clock::now()>=deadline_){finish(Status::timeout);return;}
        if(!startup_->readyToVerify()){finish(Status::unavailable);return;}
        int peer=accept(listener_,nullptr,nullptr);
        if(peer<0){if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)finish(Status::io,errno);return;}
        const auto capture=Pro40::SocketPeerCapture::capture(peer,startup_->retainedChild()->child().pid());
        if(close(peer)){endpointError_=errno;finish(Status::io,errno);return;}
        if(startup_->stopQuery().requested()){finish(Status::stopped);return;}
        if(std::chrono::steady_clock::now()>=deadline_){finish(Status::timeout);return;}
        state_.capture=capture;finish(capture?Status::captured:Status::invalid);
    }
public:
    ~WorkerSocketCaptureOwner(){(void)cleanup();} // Not terminal evidence or child reap.
    static std::shared_ptr<WorkerSocketCaptureOwner> create(dispatch_queue_t q,std::function<void(WorkerSocketCaptureReport)> report){
        if(!q || !report)return {};
        return std::shared_ptr<WorkerSocketCaptureOwner>(new WorkerSocketCaptureOwner(q,std::move(report)));
    }
    [[nodiscard]] bool prepare(){
        dispatch_assert_queue(queue_);if(prepared_ || finished_)return false;prepared_=true;
        char folder[]="/tmp/pro40-native-intro-XXXXXX";
        if(!mkdtemp(folder)){finish(WorkerSocketCaptureReport::Status::io,errno);return false;}
        directory_=folder;path_=directory_+"/peer";
        listener_=socket(AF_UNIX,SOCK_STREAM,0);
        if(listener_<0 || fcntl(listener_,F_SETFD,FD_CLOEXEC) || fcntl(listener_,F_SETFL,O_NONBLOCK)){
            finish(WorkerSocketCaptureReport::Status::io,errno);return false;
        }
        sockaddr_un address{};address.sun_family=AF_UNIX;std::memcpy(address.sun_path,path_.c_str(),path_.size()+1);
        if(bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof(address))){finish(WorkerSocketCaptureReport::Status::io,errno);return false;}
        bound_=true;
        if(listen(listener_,1)){finish(WorkerSocketCaptureReport::Status::io,errno);return false;}return true;
    }
    const std::string& path()const{dispatch_assert_queue(queue_);return path_;}
#ifdef PRO40_CAPTURE_TESTING
    void failTimerForTesting(){dispatch_assert_queue(queue_);failTimer_=true;}
#endif
    [[nodiscard]] bool start(std::shared_ptr<WorkerStartupOwner> startup,std::chrono::steady_clock::time_point deadline){
        dispatch_assert_queue(queue_);if(started_ || !prepared_ || finished_)return false;started_=true;
        startup_=std::move(startup);deadline_=deadline;
        if(!startup_ || !startup_->readyToVerify()){finish(WorkerSocketCaptureReport::Status::unavailable);return false;}
#ifdef PRO40_CAPTURE_TESTING
        if(!failTimer_)
#endif
        timer_=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,queue_);
        if(!timer_){finish(WorkerSocketCaptureReport::Status::scheduling);return false;}
        auto retained=shared_from_this();dispatch_source_set_event_handler(timer_,^{retained->tick();});
        dispatch_source_set_timer(timer_,dispatch_time(DISPATCH_TIME_NOW,0),5*NSEC_PER_MSEC,NSEC_PER_MSEC);
        dispatch_resume(timer_);return true;
    }
    void cancel(){dispatch_assert_queue(queue_);finish(WorkerSocketCaptureReport::Status::stopped);}
    WorkerSocketCaptureReport snapshot()const{dispatch_assert_queue(queue_);return state_;}
};
}
