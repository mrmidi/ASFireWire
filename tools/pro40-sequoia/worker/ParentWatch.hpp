// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <atomic>
#include <cerrno>
#include <poll.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
namespace Pro40 {
// Parent holds the sole write end; EOF, unexpected data or a channel error
// requests cooperative stop. This thread never touches native resources.
class ParentWatch {
    std::atomic<bool> stop_{false};
    std::atomic<bool> done_{false};
    std::thread monitor_;
public:
    ~ParentWatch() {
        done_.store(true,std::memory_order_relaxed);
        if(monitor_.joinable())monitor_.join();
    }
    bool start(int fd) {
        struct stat info{};
        if(monitor_.joinable() || fstat(fd,&info)<0 || !S_ISFIFO(info.st_mode))return false;
        try {
            monitor_=std::thread([this,fd] {
                while(!done_.load(std::memory_order_relaxed)) {
                    pollfd event{fd,POLLIN,0};
                    const int result=poll(&event,1,50);
                    if(result<0 && errno==EINTR)continue;
                    if(result==0)continue;
                    // The liveness channel carries no data. Any readiness or
                    // failure means its contract ended; no blocking read needed.
                    stop_.store(true,std::memory_order_relaxed);
                    return;
                }
            });
        } catch(...) {return false;}
        return true;
    }
    bool requested() const noexcept {return stop_.load(std::memory_order_relaxed);}
};
}
