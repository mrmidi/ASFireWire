// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../NativeProcessBirth.hpp"
#include <Security/Security.h>
#include <xpc/xpc.h>
#include <memory>
#include <string>
#include <type_traits>
#include <strings.h>
namespace Pro40Lab {
struct AuthenticatedVerifierPeer {
    NativeProcessBirth birth;
    uint64_t observedNS=0; // assigned by the connection owner after validation
};
inline bool sameVerifierBirth(const NativeProcessBirth& a,const NativeProcessBirth& b){
    if(!std::memchr(a.boot,0,sizeof(a.boot)) || !std::memchr(b.boot,0,sizeof(b.boot)) ||
        std::strlen(a.boot)!=36 || std::strlen(b.boot)!=36 || a.pid<=0 || b.pid<=0 ||
        !a.seconds || !b.seconds || a.microseconds>=1000000 || b.microseconds>=1000000)return false;
    return a.pid==b.pid && !strcasecmp(a.boot,b.boot) && a.seconds==b.seconds && a.microseconds==b.microseconds;
}
namespace VerifierPeerDetail {
struct Release {void operator()(CFTypeRef p)const{CFRelease(p);}};
template<class T>using Owned=std::unique_ptr<std::remove_pointer_t<T>,Release>;
}
// Non-real-time only. The message must be a real received XPC dictionary. SDK
// SecCode.h:193-213 binds its code object to the kernel-associated audit token;
// SecCode.h:214-238 validates that dynamic object, not a pathname or PID alone.
// connectionPID is supplied by the owning connection's xpc_connection_get_pid,
// never by a message field or a caller-selected unrelated process.
inline std::optional<NativeProcessBirth> authenticatedVerifierBirth(
    xpc_object_t message,pid_t connectionPID,const std::string& trustedRequirement){
    if(connectionPID<=0 || trustedRequirement.empty() || xpc_get_type(message)!=XPC_TYPE_DICTIONARY)return {};
    SecCodeRef rawCode=nullptr;
    if(SecCodeCreateWithXPCMessage(message,kSecCSDefaultFlags,&rawCode)!=errSecSuccess)return {};
    VerifierPeerDetail::Owned<SecCodeRef> code(rawCode);
    VerifierPeerDetail::Owned<CFStringRef> text(CFStringCreateWithCString(nullptr,trustedRequirement.c_str(),kCFStringEncodingUTF8));
    if(!text)return {};
    SecRequirementRef rawRequirement=nullptr;
    if(SecRequirementCreateWithString(text.get(),kSecCSDefaultFlags,&rawRequirement)!=errSecSuccess)return {};
    VerifierPeerDetail::Owned<SecRequirementRef> requirement(rawRequirement);
    const auto before=NativeProcessBirth::read(connectionPID);
    if(!before || SecCodeCheckValidity(code.get(),kSecCSNoNetworkAccess,requirement.get())!=errSecSuccess)return {};
    const auto after=NativeProcessBirth::read(connectionPID);
    if(!after || !sameVerifierBirth(*before,*after) ||
        SecCodeCheckValidity(code.get(),kSecCSNoNetworkAccess,requirement.get())!=errSecSuccess)return {};
    return after;
}
}
