// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AdmissionWait.hpp"
#include "WorkerIntroduction.hpp"
#include <charconv>
#include <optional>
#include <string_view>
namespace Pro40 {
struct ManagedWorkerPeer {
    std::array<uint8_t,16> requestID{};
    std::optional<SocketPeerCapture> parent;
};
// Explicit managed-entry contract. Fixed inherited fd 10 is owned here; argv
// carries the controller nonce. Neither is proof of controller identity. The
// controller must verify the retained child before sending the release frame.
// argv also supplies the private listener path; normal bootstrap is inherited.
inline std::optional<NativeSessionResult> managedWorkerAdmission(
    int &argc,char **&argv,StopQuery stop,std::chrono::steady_clock::time_point deadline,
    ManagedWorkerPeer *peer=nullptr){
    if(peer)*peer={}; // Never leave a previous launch's identity on refusal.
    const pid_t launchParent=getppid();
    constexpr int fd=10;
    auto refuse=[&](int code){(void)close(fd);return std::optional<NativeSessionResult>{{code,{}}};};
    if(argc<5 || std::string_view(argv[1])!="--managed")return refuse(64);
    const std::string_view nonce(argv[2]);std::array<uint8_t,16> request{};
    if(nonce.size()!=32)return refuse(64);
    for(size_t i=0;i<request.size();++i){
        unsigned byte=0;const auto parsed=std::from_chars(nonce.data()+2*i,nonce.data()+2*i+2,byte,16);
        if(parsed.ec!=std::errc{} || parsed.ptr!=nonce.data()+2*i+2)return refuse(64);
        request[i]=uint8_t(byte);
    }
    bool nonzero=false;for(auto byte:request)nonzero|=byte!=0;
    if(!nonzero)return refuse(64);
    const int flags=fcntl(fd,F_GETFL);struct stat info{};
    if(flags<0)return refuse(74);
    if(!(flags&O_NONBLOCK) || (flags&O_ACCMODE)==O_WRONLY || fstat(fd,&info) || !S_ISFIFO(info.st_mode))return refuse(64);
    WorkerIntroduction introduction;
    auto result=introduction.connect(argv[3],stop,deadline);
    std::optional<SocketPeerCapture> parent;
    if(peer && result==AdmissionWaitResult::admitted){
        parent=introduction.peer(launchParent);
        if(!parent || launchParent<=1 || getppid()!=launchParent)result=AdmissionWaitResult::rejected;
    }
    if(result==AdmissionWaitResult::admitted){AdmissionWait gate;result=gate.wait(fd,request,stop,deadline);}
    const bool introductionClosed=introduction.close();
    const int closed=close(fd); // Never retry an ambiguous close.
    if(closed || !introductionClosed)return NativeSessionResult{74,{}};
    if(stop.requested() || result==AdmissionWaitResult::cancelled)return NativeSessionResult{2,{}};
    if(result==AdmissionWaitResult::admitted){
        if(peer){if(getppid()!=launchParent)return NativeSessionResult{64,{}};*peer={request,parent};}
        argc-=3;argv+=3;return {};
    }
    if(result==AdmissionWaitResult::timedOut)return NativeSessionResult{75,{}};
    return NativeSessionResult{result==AdmissionWaitResult::ioError?74:64,{}};
}
}
