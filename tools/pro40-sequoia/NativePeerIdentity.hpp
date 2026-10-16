// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <Security/Security.h>
#include <mach/message.h>
#include <cstring>
#include <memory>
#include <type_traits>

namespace Pro40Audio {
namespace IdentityDetail {
struct Release {void operator()(CFTypeRef value) const noexcept {CFRelease(value);}};
template<class T> using Owned=std::unique_ptr<std::remove_pointer_t<T>,Release>;
}
// Non-RT synchronous prototype. Caller supplies trusted policy, never a
// requirement learned from the incoming peer. No timeout/preemption guarantee.
// Public API attribute representation cross-checked against Apple's Security
// OSX/libsecurity_codesigning/lib/SecCode.cpp, SecCodeCreateWithAuditToken:
// kSecGuestAttributeAudit is CFData containing the complete opaque audit token.
// No private API is called. Token must come from a successful kernel receive.
[[nodiscard]] inline OSStatus verifyNativePeerIdentity(
    const audit_token_t &token,SecRequirementRef requirement) noexcept {
    const audit_token_t invalid=INVALID_AUDIT_TOKEN_VALUE;
    if(!requirement || std::memcmp(&token,&invalid,sizeof(token))==0)return errSecParam;
    IdentityDetail::Owned<CFDataRef> data(CFDataCreate(nullptr,reinterpret_cast<const UInt8 *>(&token),sizeof(token)));
    if(!data)return errSecAllocate;
    const void *key=kSecGuestAttributeAudit,*value=data.get();
    IdentityDetail::Owned<CFDictionaryRef> attributes(CFDictionaryCreate(nullptr,&key,&value,1,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks));
    if(!attributes)return errSecAllocate;
    SecCodeRef code=nullptr;
    const auto found=SecCodeCopyGuestWithAttributes(nullptr,attributes.get(),kSecCSDefaultFlags,&code);
    if(found!=errSecSuccess)return found;
    IdentityDetail::Owned<SecCodeRef> ownedCode(code);
    return SecCodeCheckValidity(ownedCode.get(),kSecCSNoNetworkAccess,requirement);
}
}
