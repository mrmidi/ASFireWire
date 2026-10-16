// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <utility>
namespace Pro40Audio {
struct NativeChildCalls {
    pid_t (*wait)(pid_t,int *,int)=::waitpid;
    int (*signal)(pid_t,int)=::kill;
};
// Single controller, exclusive waitpid ownership. No SIGCHLD auto-reaping or
// other waiter may consume this child. Move the record into the caller's outcome
// when cleanup is unresolved. Destruction neither blocks nor signals: a numeric
// PID after lost wait ownership must never be used to kill a possibly reused PID.
class NativeChildProcess {
public:
    enum class State {empty,running,reaped,ownershipUnknown};
private:
    pid_t pid_=0;
    State state_=State::empty;
    int exitStatus_=0,waitError_=0,signalError_=0;
    bool stopAttempted_=false;
    NativeChildCalls calls_;
    void recordExit(int status)noexcept{
        if(WIFEXITED(status) || WIFSIGNALED(status)){exitStatus_=status;state_=State::reaped;}
        else {waitError_=EPROTO;state_=State::ownershipUnknown;}
    }
public:
    explicit NativeChildProcess(pid_t pid,NativeChildCalls calls={}):
        pid_(pid),state_(pid>0?State::running:State::empty),calls_(calls){}
    NativeChildProcess(const NativeChildProcess &)=delete;
    NativeChildProcess &operator=(const NativeChildProcess &)=delete;
    NativeChildProcess(NativeChildProcess &&other)noexcept:
        pid_(std::exchange(other.pid_,0)),state_(std::exchange(other.state_,State::empty)),
        exitStatus_(other.exitStatus_),waitError_(other.waitError_),signalError_(other.signalError_),
        stopAttempted_(other.stopAttempted_),calls_(other.calls_){}
    NativeChildProcess &operator=(NativeChildProcess &&)=delete;
    [[nodiscard]] State poll()noexcept{
        if(state_!=State::running)return state_;
        int status=0;const auto waited=calls_.wait(pid_,&status,WNOHANG);
        if(waited==pid_)recordExit(status);
        else if(waited<0 && errno!=EINTR){waitError_=errno;state_=State::ownershipUnknown;}
        else if(waited>0){waitError_=EPROTO;state_=State::ownershipUnknown;}
        return state_;
    }
    [[nodiscard]] State requestStop()noexcept{
        // Verify wait ownership immediately before signaling. EINTR means this
        // poll was inconclusive; postpone the stop instead of guessing ownership.
        if(state_!=State::running || stopAttempted_)return state_;
        int status=0;const auto waited=calls_.wait(pid_,&status,WNOHANG);
        if(waited==pid_){recordExit(status);return state_;}
        if(waited<0 && errno==EINTR)return state_;
        if(waited!=0){waitError_=waited<0?errno:EPROTO;state_=State::ownershipUnknown;return state_;}
        stopAttempted_=true;
        if(calls_.signal(pid_,SIGKILL)<0)signalError_=errno;
        return state_; // Even successful kill or ESRCH is NOT proof of reaping.
    }
    State state()const noexcept{return state_;}
    pid_t pid()const noexcept{return pid_;}
    int exitStatus()const noexcept{return exitStatus_;} // Valid only in reaped state.
    int waitError()const noexcept{return waitError_;}
    int signalError()const noexcept{return signalError_;}
    bool stopAttempted()const noexcept{return stopAttempted_;}
};
}
