// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuplexProvisionTransport.hpp"
#include "NativePeerIdentity.hpp"
namespace Pro40Audio::ProvisionIntroduction {
// Public service handshake. It carries only a disposable reply capability; a
// controller endpoint is sent later, over the authenticated PRIVATE endpoint.
// Both requirements come from trusted owner configuration, never the wire.
enum class Role:uint32_t {controller=1,helper=2};
inline constexpr mach_msg_id_t helloID=0x50344454,replyID=0x50344455,deniedID=0x50344456;
struct Hello {mach_msg_header_t header{};uint32_t version=1;Role role=Role::helper;uint64_t nonce=0;};
struct Reply {
    mach_msg_header_t header{};mach_msg_body_t body{};mach_msg_port_descriptor_t endpoint{};
    uint32_t version=1;Role role=Role::helper;uint64_t nonce=0;
};
struct Denied {mach_msg_header_t header{};uint32_t version=1;OSStatus status=errSecParam;uint64_t nonce=0;};
static_assert(sizeof(Hello)==40 && sizeof(Denied)==40 && sizeof(Reply)==56);
inline bool valid(Role role)noexcept{return role==Role::controller || role==Role::helper;}
struct Accepted {
    NativeMachPort receive{MACH_PORT_RIGHT_RECEIVE};
    audit_token_t token=INVALID_AUDIT_TOKEN_VALUE;
    Role role=Role::helper;
};
struct Served {
    kern_return_t transport=KERN_FAILURE;OSStatus identity=errSecParam;
    std::unique_ptr<Accepted> accepted;
};
inline Served serveOne(mach_port_t listener,SecRequirementRef controller,SecRequirementRef helper,
                       mach_msg_timeout_t timeout=1000) {
    Served result;
    if(!controller || !helper || !timeout || timeout>5000){result.transport=KERN_INVALID_ARGUMENT;return result;}
    NativeMachReceived<256> message;
    if(!message.read(listener,timeout,nativeAuditReceiveOptions)){result.transport=message.status();return result;}
    Hello hello;std::memcpy(&hello,message.bytes().data(),sizeof(hello));
    const auto &h=hello.header;auto token=nativeAuditToken(message.bytes());
    if(h.msgh_id!=helloID || h.msgh_size!=sizeof(Hello) || (h.msgh_bits&MACH_MSGH_BITS_COMPLEX) ||
       !ProvisionTransport::requestEnvelope(h,listener) || !token || hello.version!=1 || !valid(hello.role) || !hello.nonce) {
        result.transport=KERN_INVALID_VALUE;return result;
    }
    result.identity=verifyNativePeerIdentity(*token,hello.role==Role::controller?controller:helper);
    if(result.identity!=errSecSuccess) {
        Denied reply;reply.status=result.identity;reply.nonce=hello.nonce;
        ProvisionTransport::outgoing(reply.header,h.msgh_remote_port,deniedID,sizeof(reply));
        result.transport=ProvisionTransport::send(reply.header,timeout);return result;
    }
    auto accepted=std::make_unique<Accepted>();accepted->token=*token;accepted->role=hello.role;
    result.transport=accepted->receive.allocate();if(result.transport!=KERN_SUCCESS)return result;
    Reply reply;reply.role=hello.role;reply.nonce=hello.nonce;reply.body.msgh_descriptor_count=1;
    reply.endpoint.name=accepted->receive.get();reply.endpoint.type=MACH_MSG_PORT_DESCRIPTOR;
    reply.endpoint.disposition=MACH_MSG_TYPE_MAKE_SEND;
    ProvisionTransport::outgoing(reply.header,h.msgh_remote_port,replyID,sizeof(reply),true);
    result.transport=ProvisionTransport::send(reply.header,timeout);
    if(result.transport==KERN_SUCCESS)result.accepted=std::move(accepted);
    return result;
}
struct Connection {
    NativeMachPort service{MACH_PORT_RIGHT_SEND};
    audit_token_t token=INVALID_AUDIT_TOKEN_VALUE;
};
struct Answer {
    kern_return_t transport=KERN_FAILURE;
    OSStatus serverIdentity=errSecParam,clientIdentity=errSecParam;
    std::unique_ptr<Connection> connection;
    bool ok()const noexcept{return transport==KERN_SUCCESS && serverIdentity==errSecSuccess && clientIdentity==errSecSuccess && connection;}
};
inline Answer connect(mach_port_t publicService,SecRequirementRef expectedServer,Role role,uint64_t nonce,
                      mach_msg_timeout_t timeout=2000) {
    Answer answer;
    if(!MACH_PORT_VALID(publicService) || !expectedServer || !valid(role) || !nonce || !timeout || timeout>5000) {
        answer.transport=KERN_INVALID_ARGUMENT;return answer;
    }
    NativeMachPort replyPort(MACH_PORT_RIGHT_RECEIVE);answer.transport=replyPort.allocate();
    if(answer.transport!=KERN_SUCCESS)return answer;
    Hello request;request.role=role;request.nonce=nonce;
    ProvisionTransport::outgoing(request.header,publicService,helloID,sizeof(request));
    request.header.msgh_local_port=replyPort.get();request.header.msgh_bits|=MACH_MSGH_BITS(0,MACH_MSG_TYPE_MAKE_SEND);
    answer.transport=ProvisionTransport::send(request.header,timeout);if(answer.transport!=KERN_SUCCESS)return answer;
    NativeMachReceived<256> message;
    if(!message.read(replyPort.get(),timeout,nativeAuditReceiveOptions)){answer.transport=message.status();return answer;}
    mach_msg_header_t h;std::memcpy(&h,message.bytes().data(),sizeof(h));auto token=nativeAuditToken(message.bytes());
    if(h.msgh_local_port!=replyPort.get() || h.msgh_remote_port!=MACH_PORT_NULL || h.msgh_voucher_port!=MACH_PORT_NULL || !token) {
        answer.transport=KERN_INVALID_VALUE;return answer;
    }
    answer.serverIdentity=verifyNativePeerIdentity(*token,expectedServer);
    if(answer.serverIdentity!=errSecSuccess)return answer;
    if(h.msgh_id==deniedID && h.msgh_size==sizeof(Denied) && !(h.msgh_bits&MACH_MSGH_BITS_COMPLEX)) {
        Denied denied;std::memcpy(&denied,message.bytes().data(),sizeof(denied));
        if(denied.version!=1 || denied.nonce!=nonce || denied.status==errSecSuccess){answer.transport=KERN_INVALID_VALUE;return answer;}
        answer.clientIdentity=denied.status;return answer;
    }
    if(h.msgh_id!=replyID || h.msgh_size!=sizeof(Reply) || !(h.msgh_bits&MACH_MSGH_BITS_COMPLEX)) {
        answer.transport=KERN_INVALID_VALUE;return answer;
    }
    Reply reply;std::memcpy(&reply,message.bytes().data(),sizeof(reply));
    if(reply.version!=1 || reply.role!=role || reply.nonce!=nonce || reply.body.msgh_descriptor_count!=1 ||
       reply.endpoint.type!=MACH_MSG_PORT_DESCRIPTOR || reply.endpoint.disposition!=MACH_MSG_TYPE_PORT_SEND || !MACH_PORT_VALID(reply.endpoint.name)) {
        answer.transport=KERN_INVALID_VALUE;return answer;
    }
    auto connection=std::make_unique<Connection>();answer.transport=connection->service.retainSend(reply.endpoint.name);
    if(answer.transport!=KERN_SUCCESS)return answer;
    connection->token=*token;answer.connection=std::move(connection);answer.clientIdentity=errSecSuccess;
    return answer;
}
}
