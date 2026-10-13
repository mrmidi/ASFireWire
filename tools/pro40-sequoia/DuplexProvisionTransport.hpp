// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuplexProvisionRegistry.hpp"
namespace Pro40Audio::ProvisionTransport {
// Pre-authenticated private control protocol, separate from the worker's audio
// channel. A prior code-authenticated introduction must supply the PRIVATE
// service endpoint and exact original token. Never pass a named lookup port or
// learn an expected token from the requests below. Public service introduction
// is deliberately a separate owner, not implemented by this transport.
enum class Kind:uint32_t {bind=1,lookup=2,unbind=3};
inline constexpr mach_msg_id_t requestID=0x50344450,bindID=0x50344451,ackID=0x50344452,introID=0x50344453;
struct Request {mach_msg_header_t header{};uint32_t version=1;Kind kind=Kind::lookup;uint64_t nonce=0;};
struct Bind {mach_msg_header_t header{};mach_msg_body_t body{};mach_msg_port_descriptor_t endpoint{};uint32_t version=1;Kind kind=Kind::bind;uint64_t nonce=0;};
struct Ack {mach_msg_header_t header{};uint32_t version=1;DuplexProvisionRegistry::Result result=DuplexProvisionRegistry::Result::invalid;uint64_t nonce=0;};
struct Intro {mach_msg_header_t header{};mach_msg_body_t body{};mach_msg_port_descriptor_t endpoint{};uint32_t version=1,reserved=0;uint64_t nonce=0,registration=0;audit_token_t token=INVALID_AUDIT_TOKEN_VALUE;};
static_assert(sizeof(Request)==sizeof(mach_msg_header_t)+16 && sizeof(Ack)==sizeof(Request));
static_assert(sizeof(Bind)==sizeof(Request)+sizeof(mach_msg_body_t)+sizeof(mach_msg_port_descriptor_t));
static_assert(sizeof(Intro)==sizeof(Bind)+8+sizeof(audit_token_t));
inline kern_return_t send(mach_msg_header_t &h,mach_msg_timeout_t timeout)noexcept {
    return mach_msg(&h,MACH_SEND_MSG|MACH_SEND_TIMEOUT,h.msgh_size,0,MACH_PORT_NULL,timeout,MACH_PORT_NULL);
}
inline void outgoing(mach_msg_header_t &h,mach_port_t destination,mach_msg_id_t id,size_t size,bool complex=false)noexcept {
    h.msgh_remote_port=destination;h.msgh_id=id;h.msgh_size=mach_msg_size_t(size);
    h.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,0)|(complex?MACH_MSGH_BITS_COMPLEX:0);
}
inline bool requestEnvelope(const mach_msg_header_t &h,mach_port_t listener)noexcept {
    return h.msgh_local_port==listener && MACH_PORT_VALID(h.msgh_remote_port) &&
        MACH_MSGH_BITS_REMOTE(h.msgh_bits)==MACH_MSG_TYPE_PORT_SEND && h.msgh_voucher_port==MACH_PORT_NULL;
}
struct Served {kern_return_t transport=KERN_FAILURE;DuplexProvisionRegistry::Result result=DuplexProvisionRegistry::Result::invalid;Kind kind=Kind::lookup;};
inline Served serveOne(DuplexProvisionRegistry &registry,mach_port_t listener,audit_token_t authorizedPeer,mach_msg_timeout_t timeout=1000) {
    Served result;NativeMachReceived<256> message;
    const audit_token_t invalid=INVALID_AUDIT_TOKEN_VALUE;
    if(!timeout || timeout>5000 || !std::memcmp(&authorizedPeer,&invalid,sizeof(invalid))){result.transport=KERN_INVALID_ARGUMENT;return result;}
    if(!message.read(listener,timeout,nativeAuditReceiveOptions)){result.transport=message.status();return result;}
    mach_msg_header_t h;std::memcpy(&h,message.bytes().data(),sizeof(h));
    auto token=nativeAuditToken(message.bytes());
    if(!token || std::memcmp(&*token,&authorizedPeer,sizeof(authorizedPeer)) || !requestEnvelope(h,listener)){result.transport=KERN_INVALID_VALUE;return result;}
    uint64_t nonce=0;
    std::unique_ptr<DuplexProvisionRegistry::Introduction> introduction;
    if(h.msgh_id==bindID && h.msgh_size==sizeof(Bind) && (h.msgh_bits&MACH_MSGH_BITS_COMPLEX)) {
        Bind request;std::memcpy(&request,message.bytes().data(),sizeof(request));
        if(request.version!=1 || request.kind!=Kind::bind || !request.nonce || request.body.msgh_descriptor_count!=1 ||
           request.endpoint.type!=MACH_MSG_PORT_DESCRIPTOR || request.endpoint.disposition!=MACH_MSG_TYPE_PORT_SEND ||
           !MACH_PORT_VALID(request.endpoint.name)){result.transport=KERN_INVALID_VALUE;return result;}
        nonce=request.nonce;result.kind=Kind::bind;result.result=registry.bind(*token,nonce,request.endpoint.name);
    }else if(h.msgh_id==requestID && h.msgh_size==sizeof(Request) && !(h.msgh_bits&MACH_MSGH_BITS_COMPLEX)) {
        Request request;std::memcpy(&request,message.bytes().data(),sizeof(request));
        if(request.version!=1 || !request.nonce || (request.kind!=Kind::lookup && request.kind!=Kind::unbind)) {
            result.transport=KERN_INVALID_VALUE;return result;
        }
        nonce=request.nonce;result.kind=request.kind;
        if(request.kind==Kind::lookup) {
            auto lookup=registry.lookup(*token);result.result=lookup.result;introduction=std::move(lookup.introduction);
        }else result.result=registry.unbind(*token,nonce);
    }else {result.transport=KERN_INVALID_VALUE;return result;}
    if(introduction) {
        Intro reply;reply.nonce=nonce;reply.registration=introduction->registration;reply.token=introduction->token;
        outgoing(reply.header,h.msgh_remote_port,introID,sizeof(reply),true);reply.body.msgh_descriptor_count=1;
        reply.endpoint.name=introduction->controller.get();reply.endpoint.disposition=MACH_MSG_TYPE_COPY_SEND;reply.endpoint.type=MACH_MSG_PORT_DESCRIPTOR;
        result.transport=send(reply.header,timeout);
    }else {
        Ack reply;reply.result=result.result;reply.nonce=nonce;outgoing(reply.header,h.msgh_remote_port,ackID,sizeof(reply));
        result.transport=send(reply.header,timeout);
    }
    return result;
}
struct Answer {
    kern_return_t transport=KERN_FAILURE;
    bool peerMatched=false;
    DuplexProvisionRegistry::Result result=DuplexProvisionRegistry::Result::invalid;
    std::unique_ptr<DuplexProvisionRegistry::Introduction> introduction;
    bool ok()const noexcept{return transport==KERN_SUCCESS && peerMatched && result==DuplexProvisionRegistry::Result::ok;}
};
inline Answer exchange(mach_port_t service,audit_token_t authorizedServer,Kind kind,uint64_t nonce,
                       mach_port_t controllerReceive=MACH_PORT_NULL,mach_msg_timeout_t timeout=2000) {
    Answer answer;
    const audit_token_t invalid=INVALID_AUDIT_TOKEN_VALUE;
    if(!MACH_PORT_VALID(service) || !std::memcmp(&authorizedServer,&invalid,sizeof(invalid)) || !nonce || !timeout || timeout>5000 ||
       (kind!=Kind::bind && kind!=Kind::lookup && kind!=Kind::unbind) ||
       (kind==Kind::bind?!MACH_PORT_VALID(controllerReceive):controllerReceive!=MACH_PORT_NULL))return answer;
    NativeMachPort reply(MACH_PORT_RIGHT_RECEIVE);answer.transport=reply.allocate();
    if(answer.transport!=KERN_SUCCESS)return answer;
    if(kind==Kind::bind) {
        Bind request;request.nonce=nonce;outgoing(request.header,service,bindID,sizeof(request),true);
        request.header.msgh_local_port=reply.get();request.header.msgh_bits|=MACH_MSGH_BITS(0,MACH_MSG_TYPE_MAKE_SEND);
        request.body.msgh_descriptor_count=1;request.endpoint.name=controllerReceive;
        request.endpoint.type=MACH_MSG_PORT_DESCRIPTOR;request.endpoint.disposition=MACH_MSG_TYPE_MAKE_SEND;
        answer.transport=send(request.header,timeout);
    }else {
        Request request;request.kind=kind;request.nonce=nonce;outgoing(request.header,service,requestID,sizeof(request));
        request.header.msgh_local_port=reply.get();request.header.msgh_bits|=MACH_MSGH_BITS(0,MACH_MSG_TYPE_MAKE_SEND);
        answer.transport=send(request.header,timeout);
    }
    if(answer.transport!=KERN_SUCCESS)return answer;
    NativeMachReceived<256> message;
    if(!message.read(reply.get(),timeout,nativeAuditReceiveOptions)){answer.transport=message.status();return answer;}
    mach_msg_header_t h;std::memcpy(&h,message.bytes().data(),sizeof(h));auto sender=nativeAuditToken(message.bytes());
    if(h.msgh_local_port!=reply.get() || h.msgh_remote_port!=MACH_PORT_NULL || h.msgh_voucher_port!=MACH_PORT_NULL || !sender) {
        answer.transport=KERN_INVALID_VALUE;return answer;
    }
    // The exact process that supplied this private endpoint must answer. A
    // same-identifier replacement process cannot take over an in-flight RPC.
    answer.peerMatched=std::memcmp(&*sender,&authorizedServer,sizeof(authorizedServer))==0;
    if(!answer.peerMatched){answer.transport=KERN_INVALID_VALUE;return answer;}
    if(h.msgh_id==ackID && h.msgh_size==sizeof(Ack) && !(h.msgh_bits&MACH_MSGH_BITS_COMPLEX)) {
        Ack ack;std::memcpy(&ack,message.bytes().data(),sizeof(ack));
        if(ack.version!=1 || ack.nonce!=nonce || uint32_t(ack.result)>uint32_t(DuplexProvisionRegistry::Result::transport) ||
           (kind==Kind::lookup && ack.result==DuplexProvisionRegistry::Result::ok)) {
            answer.transport=KERN_INVALID_VALUE;return answer;
        }
        answer.result=ack.result;return answer;
    }
    if(kind!=Kind::lookup || h.msgh_id!=introID || h.msgh_size!=sizeof(Intro) || !(h.msgh_bits&MACH_MSGH_BITS_COMPLEX)) {
        answer.transport=KERN_INVALID_VALUE;return answer;
    }
    Intro intro;std::memcpy(&intro,message.bytes().data(),sizeof(intro));
    if(intro.version!=1 || intro.reserved || intro.nonce!=nonce || !intro.registration ||
       !std::memcmp(&intro.token,&invalid,sizeof(invalid)) || intro.body.msgh_descriptor_count!=1 ||
       intro.endpoint.type!=MACH_MSG_PORT_DESCRIPTOR || intro.endpoint.disposition!=MACH_MSG_TYPE_PORT_SEND || !MACH_PORT_VALID(intro.endpoint.name)) {
        answer.transport=KERN_INVALID_VALUE;return answer;
    }
    NativeMachPort retained(MACH_PORT_RIGHT_SEND);answer.transport=retained.retainSend(intro.endpoint.name);
    if(answer.transport!=KERN_SUCCESS)return answer;
    answer.introduction=std::make_unique<DuplexProvisionRegistry::Introduction>(std::move(retained),intro.token,intro.registration);
    answer.result=DuplexProvisionRegistry::Result::ok;return answer;
}
}
