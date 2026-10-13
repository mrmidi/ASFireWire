// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeVerifierConnection.hpp"
#include "../NativePeerIdentity.hpp"
#include <string_view>
namespace Pro40Lab {
// Local lab pin, embedded before signing both package executables. Never read
// from a request, environment variable, or peer-supplied file. It is deliberately
// separate from the sibling controller/helper authentication pins.
#ifdef PRO40_LAB_ORIGINAL_PEER_CDHASH
inline constexpr bool developmentOriginalPeerEnabled=true;
inline constexpr std::string_view developmentOriginalPeerHash=PRO40_LAB_ORIGINAL_PEER_CDHASH;
#else
inline constexpr bool developmentOriginalPeerEnabled=false;
inline constexpr std::string_view developmentOriginalPeerHash="";
#endif
inline constexpr bool validOriginalPeerHash(std::string_view hash){
    if(hash.size()!=40)return false;
    bool nonzero=false;
    for(char c:hash){if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;nonzero|=c!='0';}
    return nonzero;
}
static_assert(!developmentOriginalPeerEnabled || validOriginalPeerHash(developmentOriginalPeerHash));
inline std::array<uint8_t,20> developmentOriginalPolicyHash(){
    std::array<uint8_t,20> bytes{};
    if(!developmentOriginalPeerEnabled)return bytes;
    auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(size_t i=0;i<bytes.size();++i)bytes[i]=uint8_t(digit(developmentOriginalPeerHash[i*2])*16+digit(developmentOriginalPeerHash[i*2+1]));
    return bytes;
}
// Call only after running-package identity validation. No IO or verification of
// the original peer until the helper invokes check with a kernel-origin token.
inline std::optional<NativeVerifierConnection::Identity> developmentOriginalIdentity(
    bool helper,const std::optional<Pro40Audio::NativeIdentityRequest>& request={}){
    if(!developmentOriginalPeerEnabled)return {};
    NativeVerifierConnection::Identity identity;
    const auto hash=developmentOriginalPolicyHash();
    if(!helper){
        if(!request || !request->valid() || request->policyHash!=hash)return {};
        identity.request=request;return identity;
    }
    if(request)return {};
    const auto expression="cdhash H\""+std::string(developmentOriginalPeerHash)+"\"";
    Pro40Audio::IdentityDetail::Owned<CFStringRef> text(CFStringCreateWithCString(nullptr,expression.c_str(),kCFStringEncodingUTF8));
    SecRequirementRef raw=nullptr;
    if(!text || SecRequirementCreateWithString(text.get(),kSecCSDefaultFlags,&raw)!=errSecSuccess)return {};
    std::shared_ptr<std::remove_pointer_t<SecRequirementRef>> requirement(raw,[](SecRequirementRef r){CFRelease(r);});
    identity.policy=hash;
    identity.check=[requirement](const audit_token_t& token){return Pro40Audio::verifyNativePeerIdentity(token,requirement.get());};
    return identity;
}
}
