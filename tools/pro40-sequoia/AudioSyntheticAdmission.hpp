// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioSyntheticStart.hpp"
#include "HalMachProbe.hpp"
#include <mach/mach_time.h>
namespace Pro40Audio::SyntheticAdmission {
// Separate experimental protocol ID, not a fallback for diagnostic admission.
inline constexpr mach_msg_id_t startID=0x50347001,ackID=0x50347002;
struct Request {
    mach_msg_header_t header{};mach_msg_body_t body{};mach_msg_port_descriptor_t port{};
    std::array<uint8_t,AudioSyntheticStartPacket::size> payload{};
};
struct Ack {mach_msg_header_t header{};std::array<uint8_t,AudioSyntheticStartPacket::size> payload{};};
static_assert(offsetof(Request,port)==sizeof(mach_msg_header_t)+sizeof(mach_msg_body_t));
inline bool ready(const AudioSyntheticStart &start) noexcept {
    mach_timebase_info_data_t local{};
    if(mach_timebase_info(&local)!=KERN_SUCCESS || !start.matchesTimebase(local.numer,local.denom))return false;
    const auto now=mach_absolute_time();
    return now<start.origin && __uint128_t(start.origin-now)*local.numer<=__uint128_t(5000000000ULL)*local.denom;
}
// Each Mach call has a 500ms limit (includes cold service startup). A missed origin is rejected again after
// exchange. No callback use. Caller retains receive ownership and MUST destroy
// that endpoint on any failure, revoking a capability already accepted by peer.
inline kern_return_t subscribe(mach_port_t control,mach_port_t receive,const AudioSyntheticStart &start) {
    if(!MACH_PORT_VALID(receive) || !ready(start))return KERN_INVALID_ARGUMENT;
    MachProbe::ReceivePort reply;
    if(!MACH_PORT_VALID(reply.get()))return KERN_RESOURCE_SHORTAGE;
    Request request;
    if(!AudioSyntheticStartPacket::encode(start,request.payload))return KERN_INVALID_ARGUMENT;
    auto status=mach_port_insert_right(mach_task_self(),receive,receive,MACH_MSG_TYPE_MAKE_SEND);
    if(status!=KERN_SUCCESS)return status;
    auto &h=request.header;
    h.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND_ONCE)|MACH_MSGH_BITS_COMPLEX;
    h.msgh_remote_port=control;h.msgh_local_port=reply.get();h.msgh_size=sizeof(request);h.msgh_id=startID;
    request.body.msgh_descriptor_count=1;request.port.name=receive;
    request.port.type=MACH_MSG_PORT_DESCRIPTOR;request.port.disposition=MACH_MSG_TYPE_MOVE_SEND;
    status=mach_msg(&h,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(request),0,MACH_PORT_NULL,500,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS){mach_msg_destroy(&h);return status;}
    struct Received {Ack message{};mach_msg_max_trailer_t trailer{};} response;
    auto &out=response.message.header;
    status=mach_msg(&out,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(response),reply.get(),500,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS)return status;
    AudioSyntheticStart echoed;
    const bool valid=out.msgh_size==sizeof(Ack) && out.msgh_id==ackID &&
        !(out.msgh_bits&MACH_MSGH_BITS_COMPLEX) && out.msgh_remote_port==MACH_PORT_NULL &&
        AudioSyntheticStartPacket::decode(response.message.payload,echoed) && echoed==start && ready(start);
    mach_msg_destroy(&out);return valid?KERN_SUCCESS:KERN_INVALID_VALUE;
}
// Adopt send capability only after a validated request and successful timely ack.
// Outputs unchanged on failure. Ack delivery isn't proof of client acceptance;
// peer must stop on revoked send right or finite source deadline.
inline kern_return_t accept(mach_port_t control,mach_port_t &send,AudioSyntheticStart &start) {
    struct Received {Request message{};mach_msg_max_trailer_t trailer{};} received;
    auto &request=received.message;auto &h=request.header;
    auto status=mach_msg(&h,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(received),control,500,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS)return status;
    AudioSyntheticStart decoded;
    const bool valid=h.msgh_size==sizeof(Request) && h.msgh_id==startID &&
        (h.msgh_bits&MACH_MSGH_BITS_COMPLEX) && MACH_MSGH_BITS_REMOTE(h.msgh_bits)==MACH_MSG_TYPE_PORT_SEND_ONCE &&
        MACH_PORT_VALID(h.msgh_remote_port) && request.body.msgh_descriptor_count==1 &&
        request.port.type==MACH_MSG_PORT_DESCRIPTOR && request.port.disposition==MACH_MSG_TYPE_PORT_SEND &&
        MACH_PORT_VALID(request.port.name) && AudioSyntheticStartPacket::decode(request.payload,decoded) && ready(decoded);
    if(!valid){mach_msg_destroy(&h);return KERN_INVALID_VALUE;}
    Ack ack;ack.payload=request.payload;
    ack.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE,0);
    ack.header.msgh_remote_port=h.msgh_remote_port;h.msgh_remote_port=MACH_PORT_NULL;
    ack.header.msgh_size=sizeof(ack);ack.header.msgh_id=ackID;
    status=mach_msg(&ack.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(ack),0,MACH_PORT_NULL,500,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS)mach_msg_destroy(&ack.header);
    else if(!ready(decoded))status=KERN_INVALID_VALUE;
    if(status==KERN_SUCCESS){send=request.port.name;request.port.name=MACH_PORT_NULL;start=decoded;}
    mach_msg_destroy(&h);return status;
}
}
