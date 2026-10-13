// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../NativeSessionRunner.hpp"
#include <array>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
namespace Pro40 {
enum class AdmissionWaitResult {admitted,cancelled,timedOut,rejected,ioError,consumed};
// One control-thread attempt on a dedicated inherited pipe, separate from the
// ParentWatch lifeline. Caller owns/retains/closes the descriptor. No device IO.
// Exact controller-generated request ID plus EOF is a local release, NOT peer
// authentication: the controller must verify the original peer before sending it.
// This is a lab contract until production launch/descriptor ownership is wired.
class AdmissionWait final {
    bool consumed_=false;
public:
    AdmissionWait()=default;
    AdmissionWait(const AdmissionWait&)=delete;
    AdmissionWait& operator=(const AdmissionWait&)=delete;
    [[nodiscard]] AdmissionWaitResult wait(int fd,const std::array<uint8_t,16>& requestID,
        StopQuery stop,std::chrono::steady_clock::time_point deadline)noexcept{
        if(consumed_)return AdmissionWaitResult::consumed;
        consumed_=true;
        if(stop.requested())return AdmissionWaitResult::cancelled;
        bool nonzero=false;for(auto byte:requestID)nonzero|=byte!=0;
        struct stat info{};const int flags=fcntl(fd,F_GETFL);
        if(!nonzero || flags<0 || !(flags&O_NONBLOCK) || (flags&O_ACCMODE)==O_WRONLY ||
           fstat(fd,&info)<0 || !S_ISFIFO(info.st_mode))return AdmissionWaitResult::rejected;
        std::array<uint8_t,17> bytes{};size_t used=0;
        for(;;){
            if(stop.requested())return AdmissionWaitResult::cancelled;
            auto now=std::chrono::steady_clock::now();
            if(now>=deadline)return AdmissionWaitResult::timedOut;
            auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-now).count();
            pollfd event{fd,POLLIN,0};
            const int ready=poll(&event,1,int(left>20?20:left>0?left:1));
            if(ready<0){if(errno==EINTR)continue;return AdmissionWaitResult::ioError;}
            if(!ready)continue;
            if(event.revents&(POLLERR|POLLNVAL))return AdmissionWaitResult::ioError;
            const auto count=read(fd,bytes.data()+used,bytes.size()-used);
            if(count<0){if(errno==EINTR || errno==EAGAIN)continue;return AdmissionWaitResult::ioError;}
            if(count){
                used+=size_t(count);
                if(used>requestID.size())return AdmissionWaitResult::rejected;
                for(size_t i=0;i<used;++i)if(bytes[i]!=requestID[i])return AdmissionWaitResult::rejected;
                continue;
            }
            // EOF alone or a partial/extra frame is not permission. Cancellation
            // and expiration win even if a valid release just became readable.
            if(stop.requested())return AdmissionWaitResult::cancelled;
            if(std::chrono::steady_clock::now()>=deadline)return AdmissionWaitResult::timedOut;
            return used==requestID.size()?AdmissionWaitResult::admitted:AdmissionWaitResult::rejected;
        }
    }
};
}
