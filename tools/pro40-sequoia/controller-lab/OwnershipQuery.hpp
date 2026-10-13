// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../DuplexProvisionClient.hpp"
#include "JobObservation.hpp"
#include <bsm/libbsm.h>
#include <cstring>
namespace Pro40Lab::OwnershipQuery {
// A single fixed read-only operation over a code-authenticated private endpoint.
// No path, command, UID, policy or registration action comes from the caller.
inline constexpr mach_msg_id_t requestID=0x50344f51,replyID=0x50344f52;
inline constexpr size_t textLimit=16384;
enum class Status:uint32_t {ok=0,collectionFailed=1,scopeFailed=2};
struct Request {mach_msg_header_t header{};uint32_t version=1,reserved=0;uint64_t nonce=0;};
struct Reply {
    mach_msg_header_t header{};uint32_t version=1;Status status=Status::collectionFailed;
    uint64_t nonce=0;uint32_t size=0,reserved=0;
    std::array<char,textLimit> text{};
};
static_assert(sizeof(Request)==40 && offsetof(Reply,text)==48);
inline constexpr size_t padded(size_t size)noexcept{return (size+3)&~size_t(3);}
struct Answer {
    kern_return_t transport=KERN_FAILURE;
    Status status=Status::collectionFailed;
    std::string text;
    [[nodiscard]] bool ok()const noexcept{return transport==KERN_SUCCESS && status==Status::ok && !text.empty();}
};
inline bool same(const audit_token_t &a,const audit_token_t &b)noexcept{return !std::memcmp(&a,&b,sizeof(a));}
// Collector returns only a validated, driver-scoped snapshot from a successfully
// reaped fixed Apple query. Failures send no contents. Trusted server code owns
// the collector; a client cannot select what it executes.
struct Collected {Status status=Status::collectionFailed;std::string text;};
template<class Collect>
[[nodiscard]] kern_return_t serve(mach_port_t endpoint,const audit_token_t &original,uid_t owner,Collect collect,
                                 mach_msg_timeout_t timeout=1000) {
    using namespace Pro40Audio;
    NativeMachReceived<256> message;
    if(!message.read(endpoint,timeout,nativeAuditReceiveOptions))return message.status();
    Request request;std::memcpy(&request,message.bytes().data(),sizeof(request));
    const auto &h=request.header;const auto token=nativeAuditToken(message.bytes());
    if(!token || !same(*token,original) || audit_token_to_euid(*token)!=owner ||
       !ProvisionTransport::requestEnvelope(h,endpoint) || (h.msgh_bits&MACH_MSGH_BITS_COMPLEX) ||
       h.msgh_id!=requestID || h.msgh_size!=sizeof(request) || request.version!=1 || request.reserved || !request.nonce)
        return KERN_INVALID_VALUE;
    auto captured=collect();
    if(captured.status==Status::ok && (captured.text.empty() || captured.text.size()>textLimit || captured.text.find('\0')!=std::string::npos))
        captured.status=Status::scopeFailed;
    Reply reply;reply.nonce=request.nonce;reply.status=captured.status;
    if(captured.status==Status::ok){reply.size=uint32_t(captured.text.size());std::memcpy(reply.text.data(),captured.text.data(),reply.size);}
    ProvisionTransport::outgoing(reply.header,h.msgh_remote_port,replyID,offsetof(Reply,text)+padded(reply.size));
    return ProvisionTransport::send(reply.header,timeout);
}
inline Answer exchange(mach_port_t endpoint,const audit_token_t &original,uint64_t nonce,mach_msg_timeout_t timeout=3000) {
    using namespace Pro40Audio;Answer answer;
    if(!MACH_PORT_VALID(endpoint) || !nonce || !timeout || timeout>5000)return answer;
    NativeMachPort reply(MACH_PORT_RIGHT_RECEIVE);answer.transport=reply.allocate();if(answer.transport!=KERN_SUCCESS)return answer;
    Request request;request.nonce=nonce;
    ProvisionTransport::outgoing(request.header,endpoint,requestID,sizeof(request));
    request.header.msgh_local_port=reply.get();request.header.msgh_bits|=MACH_MSGH_BITS(0,MACH_MSG_TYPE_MAKE_SEND);
    answer.transport=ProvisionTransport::send(request.header,timeout);if(answer.transport!=KERN_SUCCESS)return answer;
    NativeMachReceived<sizeof(Reply)+sizeof(mach_msg_max_trailer_t)> message;
    if(!message.read(reply.get(),timeout,nativeAuditReceiveOptions)){answer.transport=message.status();return answer;}
    Reply received;std::memcpy(&received,message.bytes().data(),offsetof(Reply,text));
    const auto &h=received.header;const auto token=nativeAuditToken(message.bytes());
    if(!token || !same(*token,original) || h.msgh_local_port!=reply.get() || h.msgh_remote_port!=MACH_PORT_NULL ||
       h.msgh_voucher_port!=MACH_PORT_NULL || (h.msgh_bits&MACH_MSGH_BITS_COMPLEX) || h.msgh_id!=replyID ||
       h.msgh_size<offsetof(Reply,text) || received.version!=1 || received.nonce!=nonce || received.reserved ||
       received.size>textLimit || h.msgh_size!=offsetof(Reply,text)+padded(received.size) ||
       uint32_t(received.status)>uint32_t(Status::scopeFailed) ||
       (received.status==Status::ok?!received.size:received.size!=0)) {answer.transport=KERN_INVALID_VALUE;return answer;}
    answer.status=received.status;
    for(size_t i=received.size;i<padded(received.size);++i)
        if(message.bytes()[offsetof(Reply,text)+i]!=std::byte{0}){answer.transport=KERN_INVALID_VALUE;return answer;}
    if(received.size) {
        answer.text.assign(reinterpret_cast<const char *>(message.bytes().data()+offsetof(Reply,text)),received.size);
        if(answer.text.find('\0')!=std::string::npos){answer.text.clear();answer.transport=KERN_INVALID_VALUE;}
    }
    return answer;
}
// Public name is discovery only. Authenticate both dynamic code and root UID,
// then require the exact original server token on the private reply. The
// production adapter never falls back to an interactive sfltool subprocess.
inline Answer query(const char *service,SecRequirementRef expectedServer) {
    using namespace Pro40Audio;
    auto connection=ProvisionClient::connect(service,expectedServer,ProvisionIntroduction::Role::controller);
    if(!connection.ok() || audit_token_to_euid(connection.connection->token)!=0)return {};
    return exchange(connection.connection->service.get(),connection.connection->token,ProvisionClient::nonce());
}
}
