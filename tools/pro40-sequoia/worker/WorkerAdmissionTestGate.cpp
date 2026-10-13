// SPDX-License-Identifier: Apache-2.0
// Linked only into the actual adapter's hardware-free admission test executable.
#if !defined(PRO40_WORKER_ADMISSION_TEST) || !defined(PRO40_WORKER_TEST_LOCKS)
#error Not a production worker entry point
#endif
#include "AdmissionWait.hpp"
#include "WorkerDiagnosticTiming.hpp"
#include <charconv>
#include <cstdlib>
#include <cstdio>
#include <optional>
#include <string_view>
#ifdef PRO40_WORKER_CAPTURE_TEST
#include <mach/mach.h>
#endif
namespace Pro40 {
std::optional<NativeSessionResult> testWorkerAdmission(StopQuery stop){
    const char *value=std::getenv("PRO40_ADMISSION_TEST_FD");
    if(!value)return NativeSessionResult{64,{}};
    std::string_view text(value);int fd=-1;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),fd);
    if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || fd<=2)return NativeSessionResult{64,{}};
    std::array<uint8_t,16> requestID{};for(unsigned i=0;i<16;++i)requestID[i]=i+1;
#ifdef PRO40_WORKER_CAPTURE_TEST
    // Controller fixture supplies a fresh request ID before spawn. This is only
    // test configuration, not a production environment-based trust boundary.
    const char *nonce=std::getenv("PRO40_ADMISSION_TEST_NONCE");
    bool valid=nonce && std::string_view(nonce).size()==32;
    if(valid)for(size_t i=0;i<16;++i){
        unsigned byte=0;auto p=std::from_chars(nonce+i*2,nonce+i*2+2,byte,16);
        if(p.ec!=std::errc{} || p.ptr!=nonce+i*2+2){valid=false;break;}
        requestID[i]=uint8_t(byte);
    }
    mach_port_t port=MACH_PORT_NULL;
    if(!valid || task_get_bootstrap_port(mach_task_self(),&port)!=KERN_SUCCESS){close(fd);return NativeSessionResult{64,{}};}
    mach_msg_header_t hello{};hello.msgh_size=sizeof(hello);hello.msgh_remote_port=port;
    hello.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,0);
    const auto sent=mach_msg(&hello,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(hello),0,MACH_PORT_NULL,1000,MACH_PORT_NULL);
    const auto dropped=mach_port_deallocate(mach_task_self(),port);
    if(sent!=KERN_SUCCESS || dropped!=KERN_SUCCESS){close(fd);return NativeSessionResult{74,{}};}
#ifdef PRO40_WORKER_NATIVE_DIAGNOSTIC_TEST
    constexpr auto budget=WorkerDiagnosticTiming::admission;
#else
    constexpr auto budget=std::chrono::seconds(8);
#endif
#else
    constexpr auto budget=std::chrono::milliseconds(600);
#endif
    AdmissionWait gate;
    const auto result=gate.wait(fd,requestID,stop,std::chrono::steady_clock::now()+budget);
    // The fixture owns this dedicated inherited descriptor, never stdin/stdout.
    const int closed=close(fd);
    std::fprintf(stderr,"worker-admission-test: status=%u closed=%u\n",unsigned(result),unsigned(closed==0));
    if(closed!=0)return NativeSessionResult{74,{}};
    if(stop.requested() || result==AdmissionWaitResult::cancelled)return NativeSessionResult{2,{}};
    if(result==AdmissionWaitResult::admitted)return {};
    if(result==AdmissionWaitResult::timedOut)return NativeSessionResult{75,{}};
    return NativeSessionResult{result==AdmissionWaitResult::ioError?74:64,{}};
}
}
