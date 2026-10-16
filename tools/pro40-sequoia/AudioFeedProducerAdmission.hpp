// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "HalMachProbe.hpp"

namespace Pro40Audio::ProducerAdmission {
// Diagnostic protocol: client keeps receive ownership and transfers only a send
// capability to the producer. No identity/authentication policy is implied.
inline constexpr mach_msg_id_t subscribeID=0x50346001,ackID=0x50346002;
struct Subscribe {
    mach_msg_header_t header{};mach_msg_body_t body{};
    mach_msg_port_descriptor_t port{};
    uint64_t nonce=0,epoch=0;
};
static_assert(offsetof(Subscribe,port)==sizeof(mach_msg_header_t)+sizeof(mach_msg_body_t));
inline kern_return_t subscribe(mach_port_t control,mach_port_t receive,uint64_t epoch,uint64_t nonce) {
    if(!epoch || !MACH_PORT_VALID(receive))return KERN_INVALID_ARGUMENT;
    MachProbe::ReceivePort reply;
    if(!MACH_PORT_VALID(reply.get()))return KERN_RESOURCE_SHORTAGE;
    auto status=mach_port_insert_right(mach_task_self(),receive,receive,MACH_MSG_TYPE_MAKE_SEND);
    if(status!=KERN_SUCCESS)return status;
    Subscribe request;
    auto &h=request.header;
    h.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND_ONCE)|MACH_MSGH_BITS_COMPLEX;
    h.msgh_remote_port=control;h.msgh_local_port=reply.get();h.msgh_size=sizeof(request);h.msgh_id=subscribeID;
    request.body.msgh_descriptor_count=1;request.port.name=receive;
    request.port.type=MACH_MSG_PORT_DESCRIPTOR;request.port.disposition=MACH_MSG_TYPE_MOVE_SEND;
    request.nonce=nonce;request.epoch=epoch;
    status=mach_msg(&h,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(request),0,MACH_PORT_NULL,1000,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS){mach_msg_destroy(&h);return status;}
    MachProbe::Received response;auto &out=response.message.header;
    status=mach_msg(&out,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(response),reply.get(),3000,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS)return status;
    const bool valid=out.msgh_size==sizeof(MachProbe::Message) && out.msgh_id==ackID &&
        !(out.msgh_bits&MACH_MSGH_BITS_COMPLEX) && out.msgh_remote_port==MACH_PORT_NULL && response.message.nonce==nonce;
    mach_msg_destroy(&out);return valid?KERN_SUCCESS:KERN_INVALID_VALUE;
}
// Output send right is owned by the caller only on success. Failure destroys all
// transferred capabilities; output parameters remain unchanged.
inline kern_return_t accept(mach_port_t control,mach_port_t &send,uint64_t &epoch) {
    struct Received {Subscribe message{};mach_msg_max_trailer_t trailer{};} received;
    auto &request=received.message;auto &h=request.header;
    auto status=mach_msg(&h,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(received),control,5000,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS)return status;
    const bool valid=h.msgh_size==sizeof(Subscribe) && h.msgh_id==subscribeID &&
        (h.msgh_bits&MACH_MSGH_BITS_COMPLEX) && MACH_MSGH_BITS_REMOTE(h.msgh_bits)==MACH_MSG_TYPE_PORT_SEND_ONCE &&
        MACH_PORT_VALID(h.msgh_remote_port) && request.body.msgh_descriptor_count==1 &&
        request.port.type==MACH_MSG_PORT_DESCRIPTOR && request.port.disposition==MACH_MSG_TYPE_PORT_SEND &&
        MACH_PORT_VALID(request.port.name) && request.epoch;
    if(!valid){mach_msg_destroy(&h);return KERN_INVALID_VALUE;}
    MachProbe::Message reply;
    reply.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE,0);
    reply.header.msgh_remote_port=h.msgh_remote_port;h.msgh_remote_port=MACH_PORT_NULL;
    reply.header.msgh_id=ackID;reply.header.msgh_size=sizeof(reply);reply.nonce=request.nonce;
    status=mach_msg(&reply.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(reply),0,MACH_PORT_NULL,1000,MACH_PORT_NULL);
    if(status==KERN_SUCCESS) {send=request.port.name;epoch=request.epoch;request.port.name=MACH_PORT_NULL;}
    else mach_msg_destroy(&reply.header);
    mach_msg_destroy(&h);return status;
}
}
