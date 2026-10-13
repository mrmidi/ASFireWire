// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeMachOwnership.hpp"
#include "NativeAuditTrailer.hpp"
#include "NativePeerIdentity.hpp"
namespace Pro40Audio::ProvisioningProbe {
// Authentication experiment only. No audio, endpoint grant, launch authority,
// worker or persistent connection. Security calls are synchronous and non-RT.
inline constexpr char serviceName[]="local.pro40.provisioning-probe";
inline constexpr mach_msg_id_t requestID=0x50344440,replyID=0x50344441;
struct Message {mach_msg_header_t header{};uint32_t version=1;int32_t status=0;uint64_t nonce=0;};
static_assert(sizeof(Message)==sizeof(mach_msg_header_t)+16);
struct Result {
    kern_return_t transport=KERN_FAILURE;
    OSStatus serverIdentity=errSecParam,clientIdentity=errSecParam;
    bool passed()const noexcept{return transport==KERN_SUCCESS && serverIdentity==errSecSuccess && clientIdentity==errSecSuccess;}
};
inline IdentityDetail::Owned<SecRequirementRef> requirement(CFStringRef trustedText) {
    SecRequirementRef value=nullptr;
    if(!trustedText || SecRequirementCreateWithString(trustedText,kSecCSDefaultFlags,&value)!=errSecSuccess)return {};
    return IdentityDetail::Owned<SecRequirementRef>(value);
}
inline Result exchange(mach_port_t service,SecRequirementRef expectedServer,uint64_t nonce,mach_msg_timeout_t timeout=500) {
    Result result;
    if(!MACH_PORT_VALID(service) || !expectedServer || !nonce || !timeout)return result;
    NativeMachPort reply(MACH_PORT_RIGHT_RECEIVE);
    result.transport=reply.allocate();if(result.transport!=KERN_SUCCESS)return result;
    Message request;request.nonce=nonce;request.header.msgh_size=sizeof(request);request.header.msgh_id=requestID;
    request.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND);
    request.header.msgh_remote_port=service;request.header.msgh_local_port=reply.get();
    result.transport=mach_msg(&request.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(request),0,MACH_PORT_NULL,timeout,MACH_PORT_NULL);
    if(result.transport!=KERN_SUCCESS)return result;
    NativeMachReceived<256> received;
    if(!received.read(reply.get(),timeout,nativeAuditReceiveOptions)){result.transport=received.status();return result;}
    Message response;std::memcpy(&response,received.bytes().data(),sizeof(response));
    const auto &h=response.header;auto token=nativeAuditToken(received.bytes());
    if(h.msgh_size!=sizeof(response) || h.msgh_id!=replyID || (h.msgh_bits&MACH_MSGH_BITS_COMPLEX) ||
       h.msgh_local_port!=reply.get() || h.msgh_remote_port!=MACH_PORT_NULL || h.msgh_voucher_port!=MACH_PORT_NULL ||
       response.version!=1 || response.nonce!=nonce || !token){result.transport=KERN_INVALID_VALUE;return result;}
    result.serverIdentity=verifyNativePeerIdentity(*token,expectedServer);
    // An unverified sender's claimed verification result is never evidence.
    if(result.serverIdentity==errSecSuccess)result.clientIdentity=response.status;
    return result;
}
struct Served {kern_return_t transport=KERN_FAILURE;OSStatus clientIdentity=errSecParam;bool received=false;};
inline Served serveOne(mach_port_t listener,SecRequirementRef expectedClient,mach_msg_timeout_t timeout=500) {
    Served result;NativeMachReceived<256> received;
    if(!expectedClient || !timeout)return result;
    if(!received.read(listener,timeout,nativeAuditReceiveOptions)){result.transport=received.status();return result;}
    result.received=true;Message request;std::memcpy(&request,received.bytes().data(),sizeof(request));
    const auto &h=request.header;auto token=nativeAuditToken(received.bytes());
    if(h.msgh_size!=sizeof(request) || h.msgh_id!=requestID || (h.msgh_bits&MACH_MSGH_BITS_COMPLEX) ||
       h.msgh_local_port!=listener || !MACH_PORT_VALID(h.msgh_remote_port) ||
       MACH_MSGH_BITS_REMOTE(h.msgh_bits)!=MACH_MSG_TYPE_PORT_SEND || h.msgh_voucher_port!=MACH_PORT_NULL ||
       request.version!=1 || request.status || !request.nonce || !token){result.transport=KERN_INVALID_VALUE;return result;}
    result.clientIdentity=verifyNativePeerIdentity(*token,expectedClient);
    Message response;response.status=result.clientIdentity;response.nonce=request.nonce;
    response.header.msgh_id=replyID;response.header.msgh_size=sizeof(response);
    response.header.msgh_remote_port=h.msgh_remote_port;response.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,0);
    result.transport=mach_msg(&response.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(response),0,MACH_PORT_NULL,timeout,MACH_PORT_NULL);
    return result;
}
}
