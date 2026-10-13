// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AdmissionWait.hpp"
#include "SocketPeerCapture.hpp"
#include <cstring>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
namespace Pro40 {
// One owned connection. No namespace replacement, payload identity or retries.
// Keep connected until admission has finished; close explicitly before entry.
class WorkerIntroduction final {
    int fd_=-1;
    bool attempted_=false;
public:
    WorkerIntroduction()=default;
    WorkerIntroduction(const WorkerIntroduction&)=delete;
    WorkerIntroduction& operator=(const WorkerIntroduction&)=delete;
    ~WorkerIntroduction(){(void)close();}
    // Capture the already-connected launch parent's kernel identity. This is
    // provenance only; the inherited one-shot release pipe still gates entry.
    [[nodiscard]] std::optional<SocketPeerCapture> peer(pid_t parent)const noexcept{
        return SocketPeerCapture::capture(fd_,parent);
    }
    [[nodiscard]] bool close()noexcept{
        const int fd=fd_;fd_=-1;return fd<0 || ::close(fd)==0;
    }
    [[nodiscard]] AdmissionWaitResult connect(std::string_view path,StopQuery stop,
        std::chrono::steady_clock::time_point deadline)noexcept{
        if(attempted_)return AdmissionWaitResult::consumed;
        attempted_=true;
        sockaddr_un address{};address.sun_family=AF_UNIX;
        if(path.empty() || path.front()!='/' || path.size()>=sizeof(address.sun_path) ||
           path.find('\0')!=std::string_view::npos)return AdmissionWaitResult::rejected;
        if(stop.requested())return AdmissionWaitResult::cancelled;
        if(std::chrono::steady_clock::now()>=deadline)return AdmissionWaitResult::timedOut;
        std::memcpy(address.sun_path,path.data(),path.size());
        fd_=socket(AF_UNIX,SOCK_STREAM,0);
        if(fd_<0 || fcntl(fd_,F_SETFD,FD_CLOEXEC)<0 || fcntl(fd_,F_SETFL,O_NONBLOCK)<0)
            return AdmissionWaitResult::ioError;
        if(::connect(fd_,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){
            if(errno!=EINPROGRESS)return AdmissionWaitResult::ioError;
            for(;;){
                if(stop.requested())return AdmissionWaitResult::cancelled;
                const auto now=std::chrono::steady_clock::now();
                if(now>=deadline)return AdmissionWaitResult::timedOut;
                const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();
                pollfd pending{fd_,POLLOUT,0};
                const auto ready=poll(&pending,1,int(left>20?20:left>0?left:1));
                if(ready<0){if(errno==EINTR)continue;return AdmissionWaitResult::ioError;}
                if(!ready)continue;
                int error=0;socklen_t size=sizeof(error);
                if(getsockopt(fd_,SOL_SOCKET,SO_ERROR,&error,&size) || size!=sizeof(error) || error ||
                   !(pending.revents&POLLOUT))return AdmissionWaitResult::ioError;
                break;
            }
        }
        if(stop.requested())return AdmissionWaitResult::cancelled;
        return std::chrono::steady_clock::now()>=deadline?AdmissionWaitResult::timedOut:AdmissionWaitResult::admitted;
    }
};
}
