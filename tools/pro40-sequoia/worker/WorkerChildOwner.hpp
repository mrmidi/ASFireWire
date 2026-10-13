// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../NativeChildProcess.hpp"
#include "../NativeSessionRunner.hpp"
#include <array>
#include <fcntl.h>
#include <sys/stat.h>
#include <utility>
namespace Pro40 {
// Serial controller ownership after spawn. Transfers exclusive waitpid ownership
// and two distinct, owned pipe writers (>2). No other waiter/closer may use them.
// Caller retains this object while child state is running/unknown; destruction
// closes pipes but does not wait, signal, or claim the child was reaped.
class WorkerChildOwner final {
    Pro40Audio::NativeChildProcess child_;
    int life_=-1,release_=-1;
    bool ready_=false,attempted_=false;
    int closeError_=0;
    void closeFD(int &fd)noexcept{
        const int owned=std::exchange(fd,-1);
        // Never retry a numeric descriptor after an ambiguous close failure.
        if(owned>=0 && close(owned)<0 && !closeError_)closeError_=errno;
    }
    static bool pipeWriter(int fd,bool nonblocking)noexcept{
        struct stat info{};const int flags=fcntl(fd,F_GETFL);
        return fd>2 && flags>=0 && (flags&O_ACCMODE)==O_WRONLY &&
            (!nonblocking || (flags&O_NONBLOCK)) && fstat(fd,&info)==0 && S_ISFIFO(info.st_mode);
    }
public:
    enum class Release {sent,refused,ioError};
    WorkerChildOwner(Pro40Audio::NativeChildProcess child,int life,int release)noexcept:
        child_(std::move(child)),life_(life>2?life:-1),release_(release>2 && release!=life?release:-1){
        ready_=child_.state()==Pro40Audio::NativeChildProcess::State::running &&
            pipeWriter(life_,false) && pipeWriter(release_,true) &&
            fcntl(release_,F_SETNOSIGPIPE,1)==0;
    }
    WorkerChildOwner(const WorkerChildOwner&)=delete;
    WorkerChildOwner& operator=(const WorkerChildOwner&)=delete;
    ~WorkerChildOwner(){cancel();}
    // Local release ONLY: invoke inside fresh verified permission. This class
    // neither authenticates the worker nor proves continuing parent/child life.
    [[nodiscard]] Release release(const std::array<uint8_t,16>& requestID,StopQuery stop={})noexcept{
        if(attempted_)return Release::refused;
        attempted_=true;
        bool nonzero=false;for(auto byte:requestID)nonzero|=byte!=0;
        if(!ready_ || !nonzero || stop.requested() ||
           child_.poll()!=Pro40Audio::NativeChildProcess::State::running){
            closeFD(release_);return Release::refused;
        }
        if(stop.requested()){closeFD(release_);return Release::refused;}
        // <= PIPE_BUF on a nonblocking pipe: short/error sends never authorize
        // a retry. Worker requires exact framing and EOF before it can enter.
        const auto sent=write(release_,requestID.data(),requestID.size());
        closeFD(release_);
        return sent==ssize_t(requestID.size()) && !closeError_?Release::sent:Release::ioError;
    }
    void denyRelease()noexcept{attempted_=true;closeFD(release_);}
    void cancel()noexcept{attempted_=true;closeFD(life_);closeFD(release_);}
    [[nodiscard]] Pro40Audio::NativeChildProcess::State poll()noexcept{return child_.poll();}
    const Pro40Audio::NativeChildProcess &child()const noexcept{return child_;}
    bool endpointsClosed()const noexcept{return life_<0 && release_<0 && !closeError_;}
    // Process/pipe cleanup only. A reaped child is NOT proof of native cleanup
    // or a successful session; the controller must also validate its result.
    bool cleanupComplete()const noexcept{
        return endpointsClosed() && child_.state()==Pro40Audio::NativeChildProcess::State::reaped &&
            !child_.waitError() && !child_.signalError();
    }
    int closeError()const noexcept{return closeError_;}
};
}
