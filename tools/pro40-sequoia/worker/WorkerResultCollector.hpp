// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "WorkerChildOwner.hpp"
#include "WorkerResultProtocol.hpp"
#include <chrono>
#include <memory>
namespace Pro40 {
// Serial owner-queue polling; no sleeps, signals, or blocking reads. Owns the
// result reader and retains the SAME worker through faults/timeouts. Caller must
// keep polling/retain this owner until cleanup completes or explicit recovery.
class WorkerResultCollector final {
public:
    using Clock=std::chrono::steady_clock;
    enum class Fault {none,invalidPipe,io,protocol,timeout,ownership,stoppedSuccess};
private:
    std::shared_ptr<WorkerChildOwner> child_;
    int fd_=-1;
    WorkerResultProtocol protocol_;
    std::optional<Clock::time_point> deadline_;
    Fault fault_=Fault::none;
    bool eof_=false,finished_=false,stopped_=false,closeFailed_=false;
    std::optional<NativeSessionResult> result_;
    void fault(Fault reason)noexcept{
        if(fault_==Fault::none)fault_=reason;
        if(child_)child_->cancel();
    }
    void closeReader()noexcept{
        const int fd=std::exchange(fd_,-1);
        if(fd>=0 && close(fd)<0){closeFailed_=true;fault(Fault::io);}
    }
public:
    // Transfers fd (>2), which must be a dedicated nonblocking FIFO reader.
    WorkerResultCollector(std::shared_ptr<WorkerChildOwner> child,int fd,std::optional<Clock::time_point> deadline)noexcept:
        child_(std::move(child)),fd_(fd>2?fd:-1),deadline_(deadline){
        struct stat info{};const int flags=fcntl(fd_,F_GETFL);
        if(!child_ || flags<0 || !(flags&O_NONBLOCK) || (flags&O_ACCMODE)!=O_RDONLY ||
           fstat(fd_,&info)<0 || !S_ISFIFO(info.st_mode)){
            fault(Fault::invalidPipe);closeReader();
        }
    }
    WorkerResultCollector(const WorkerResultCollector&)=delete;
    WorkerResultCollector& operator=(const WorkerResultCollector&)=delete;
    ~WorkerResultCollector(){if(child_ && !finished_)child_->cancel();closeReader();}
    void poll(StopQuery stop={})noexcept{
        if(finished_)return;
        if(stop.requested()){stopped_=true;if(child_)child_->cancel();}
        if(deadline_ && Clock::now()>=*deadline_)fault(Fault::timeout);
        std::array<char,4096> bytes{};
        // Continue draining after a fault so a live writer cannot block on a
        // full result pipe. Per-call budget is 32KiB, including discarded data.
        for(unsigned reads=0;fd_>=0 && reads<8;++reads){
            const auto count=read(fd_,bytes.data(),bytes.size());
            if(count>0){protocol_.feed(std::span(bytes).first(size_t(count)));
                if(protocol_.failed())fault(Fault::protocol);continue;}
            if(count==0){eof_=true;protocol_.end();if(protocol_.failed())fault(Fault::protocol);closeReader();break;}
            if(errno==EINTR)continue;
            if(errno==EAGAIN)break;
            fault(Fault::io);closeReader();break;
        }
        if(!child_)return;
        const auto state=child_->poll();
        if(state==Pro40Audio::NativeChildProcess::State::ownershipUnknown){fault(Fault::ownership);return;}
        if(state!=Pro40Audio::NativeChildProcess::State::reaped || fd_>=0)return;
        child_->cancel();
        if(!child_->cleanupComplete() || closeFailed_)fault(Fault::io);
        if(fault_==Fault::none){
            result_=protocol_.result(child_->child());
            if(!eof_ || !result_)fault(Fault::protocol);
            else if(stopped_ && result_->accepted())fault(Fault::stoppedSuccess);
        }
        if(fault_!=Fault::none)result_.reset();
        finished_=true;
    }
    Fault failure()const noexcept{return fault_;}
    bool finished()const noexcept{return finished_;}
    bool cleanupComplete()const noexcept{return finished_ && !closeFailed_ && child_ && child_->cleanupComplete();}
    std::optional<NativeSessionResult> result()const noexcept{return finished_?result_:std::nullopt;}
    std::shared_ptr<WorkerChildOwner> retainedChild()const noexcept{return child_;}
};
}
