// SPDX-License-Identifier: Apache-2.0
#pragma once
// Non-RT laboratory supervisor. NOT production lifecycle/provisioning code.
#include "NativeIdentityCodec.hpp"
#include "NativeIdentityPreparation.hpp"
#include "NativeIdentityOutcome.hpp"
#include "NativePeerIdentity.hpp"
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <string>
extern char **environ;
namespace Pro40Audio {
inline uint64_t identityLabNow(){
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline bool identityLabHelperPin(const char *path,const std::string &pin){
    // Unbounded synchronous preflight; elapsed time counts, but it cannot be
    // interrupted here. Path check has a launch TOCTOU
    // gap; only suitable for the controlled lab checkout, NOT shipping trust.
    if(!path || path[0]!='/' || pin.size()!=40 || pin.find_first_not_of("0123456789abcdef")!=std::string::npos)return false;
    const std::string expression="cdhash H\""+pin+"\"";
    IdentityDetail::Owned<CFStringRef> text(CFStringCreateWithCString(nullptr,expression.c_str(),kCFStringEncodingUTF8));
    SecRequirementRef raw=nullptr;if(!text || SecRequirementCreateWithString(text.get(),0,&raw)!=errSecSuccess)return false;
    IdentityDetail::Owned<SecRequirementRef> requirement(raw);
    IdentityDetail::Owned<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(nullptr,
        reinterpret_cast<const UInt8 *>(path),std::strlen(path),false));
    SecStaticCodeRef code=nullptr;
    if(!url || SecStaticCodeCreateWithPath(url.get(),0,&code)!=errSecSuccess)return false;
    IdentityDetail::Owned<SecStaticCodeRef> owned(code);
    return SecStaticCodeCheckValidity(code,SecCSFlags(kSecCSCheckAllArchitectures)|SecCSFlags(kSecCSNoNetworkAccess),requirement.get())==errSecSuccess;
}
// Test-only counters and preparation clock injection. Never a production API.
struct IdentityLabPreparationProbe {
    unsigned preflights=0,spawns=0;
    unsigned failBeforeStep=0;
    uint64_t (*now)()=identityLabNow;
};
inline IdentityLabOutcome identityLabVerify(const char *helper,const std::string &helperPin,
    const NativeIdentityRequest &request,const char *mode="valid",uint64_t budget=3000000000,
    uint64_t cancelAfter=UINT64_MAX,IdentityLabPreparationProbe *probe=nullptr){
    const auto preparationNow=probe?probe->now:identityLabNow;
    const auto start=preparationNow();
    if(budget>UINT64_MAX-start)return {{},"invalid-budget",true,0};
    const auto deadline=start+budget;
    const auto stopReason=[&]()->const char *{
        const auto now=preparationNow();
        if(now<start)return "invalid-clock";
        if(now-start>=cancelAfter)return "cancelled";
        if(now>=deadline)return "timeout";
        return nullptr;
    };
    if(const auto reason=stopReason())return {{},reason,true,0};
    if(!request.valid())return {{},"preflight-rejected",true,0};
    if(probe)++probe->preflights;
    const bool pinMatches=identityLabHelperPin(helper,helperPin);
    if(const auto reason=stopReason())return {{},reason,true,0};
    if(!pinMatches)return {{},"preflight-rejected",true,0};
    NativeIdentityPreparation preparation;
    const auto failPreparation=[&](const char *reason,int error=0){
        const int cleanup=preparation.close();
        return IdentityLabOutcome{{},cleanup?"resource-cleanup-failed":reason,true,0,cleanup,error};
    };
    if(const int error=preparation.prepare(probe?probe->failBeforeStep:0))return failPreparation("preparation-failed",error);
    if(const auto reason=stopReason())return failPreparation(reason);
    char *args[]={const_cast<char *>(helper),const_cast<char *>(mode),const_cast<char *>(helperPin.c_str()),nullptr};
    if(probe)++probe->spawns;
    pid_t pid=0;const auto spawned=posix_spawn(&pid,helper,preparation.actions(),preparation.attributes(),args,environ);
    const int setupCleanup=preparation.finishSpawn();
    if(spawned)return failPreparation("spawn-failed",spawned);
    NativeIdentityResultGate gate(request,deadline);
    const auto bytes=encodeIdentityRequest(request);size_t sent=0,used=0;
    std::array<uint8_t,identityResultSize+1> answer{};
    bool inputOpen=true,eof=false;NativeChildProcess child(pid);
    IdentityLabOutcome result{{},"protocol-error",false,pid};
    if(setupCleanup)result.reason="resource-cleanup-failed";
    while(!setupCleanup){
        const auto now=identityLabNow();
        if(now-start>=cancelAfter){result.reason="cancelled";gate.cancel();break;}
        if(now>=deadline){result.reason="timeout";gate.cancel();break;}
        if(inputOpen){
            const auto n=write(preparation.fd(1),bytes.data()+sent,bytes.size()-sent);
            if(n>0)sent+=n;
            else if(n<0 && errno!=EAGAIN && errno!=EINTR){(void)preparation.closeFD(1);inputOpen=false;}
            if(inputOpen && sent==bytes.size()){(void)preparation.closeFD(1);inputOpen=false;}
        }
        const auto n=read(preparation.fd(2),answer.data()+used,answer.size()-used);
        if(n>0)used+=n;else if(n==0)eof=true;
        else if(errno!=EAGAIN && errno!=EINTR)break;
        if(used>identityResultSize)break;
        const auto childState=child.poll();
        if(childState==NativeChildProcess::State::ownershipUnknown){result.reason="wait-failed";gate.cancel();break;}
        if(childState==NativeChildProcess::State::reaped && eof){
            const int exitStatus=child.exitStatus();
            const auto decoded=decodeIdentityResult(std::span(answer).first(used));
            const auto finished=identityLabNow();
            const bool cancelled=finished-start>=cancelAfter;
            if(decoded)result.status=gate.consume(*decoded,WIFEXITED(exitStatus) && WEXITSTATUS(exitStatus)==0,cancelled,finished);
            result.reason=cancelled?"cancelled":finished>=deadline?"timeout":
                result.status?(result.status==0?"verified":"rejected"):"invalid-result";
            break;
        }
        usleep(1000);
    }
    const int resourceError=preparation.close();
    if(resourceError){result.status.reset();result.reason="resource-cleanup-failed";result.resourceError=resourceError;}
    if(child.state()==NativeChildProcess::State::running){
        const auto cleanupDeadline=identityLabNow()+1000000000;
        do{
            (void)child.requestStop();
            if(child.poll()!=NativeChildProcess::State::running)break;
            usleep(1000);
        }while(identityLabNow()<cleanupDeadline);
    }
    result.reaped=child.state()==NativeChildProcess::State::reaped;
    result.pendingPID=result.reaped?0:child.pid();
    if(!result.reaped || child.waitError() || child.signalError())result.status.reset();
    result.process.emplace(std::move(child));
    return result;
}
}
