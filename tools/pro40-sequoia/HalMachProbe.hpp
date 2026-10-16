// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <mach/mach.h>
#include <cstdint>
#include "AudioFeedPacket.hpp"

namespace Pro40Audio::MachProbe {
// Diagnostic only: not an audio transport or authenticated control session.
inline constexpr char serviceName[]="local.pro40.hal-ipc-lab";
inline constexpr mach_msg_id_t requestID=0x50343001,replyID=0x50343002;
inline constexpr mach_msg_id_t audioRequestID=0x50343003,audioReplyID=0x50343004;
struct Message {mach_msg_header_t header{};uint64_t nonce=0;};
struct Received {Message message{};mach_msg_max_trailer_t trailer{};};
struct AudioMessage {Message prefix{};std::array<uint8_t,AudioFeedPacket::maxBytes> packet{};};
struct AudioReceived {AudioMessage message{};mach_msg_max_trailer_t trailer{};};
static_assert(sizeof(Message)==sizeof(mach_msg_header_t)+sizeof(uint64_t));
static_assert(sizeof(AudioMessage)==sizeof(Message)+AudioFeedPacket::maxBytes);

inline int32_t fixtureSample(uint32_t frame) {
    return int32_t((uint64_t(frame)*7919)%16777216)-8388608;
}
// No publication or audio output: success means all 512 owned samples checked.
inline bool validateAudio(const AudioMessage &reply,uint64_t nonce) {
    const auto &h=reply.prefix.header;
    if((h.msgh_bits&MACH_MSGH_BITS_COMPLEX) || h.msgh_remote_port!=MACH_PORT_NULL ||
       h.msgh_size!=sizeof(AudioMessage) || h.msgh_id!=audioReplyID || reply.prefix.nonce!=nonce)return false;
    AudioFeedBatch batch;
    if(AudioFeedPacket::decode(reply.packet,batch)!=AudioFeedError::ok || batch.epoch!=nonce ||
       batch.firstFrame!=0 || batch.frames!=512)return false;
    for(uint32_t n=0;n<512;++n)if(batch.samples[n]!=fixtureSample(n))return false;
    return true;
}

class ReceivePort {
    mach_port_t port_=MACH_PORT_NULL;
public:
    ReceivePort() {if(mach_port_allocate(mach_task_self(),MACH_PORT_RIGHT_RECEIVE,&port_)!=KERN_SUCCESS)port_=MACH_PORT_NULL;}
    ~ReceivePort() {if(port_!=MACH_PORT_NULL)mach_port_mod_refs(mach_task_self(),port_,MACH_PORT_RIGHT_RECEIVE,-1);}
    ReceivePort(const ReceivePort &)=delete;
    ReceivePort &operator=(const ReceivePort &)=delete;
    mach_port_t get() const {return port_;}
};

// Caller owns a send right to service. Dedicated reply port per attempt prevents
// late responses from satisfying a later request. Two bounded phases, no retry.
// SDK mach/message.h: COPY_SEND, MAKE_SEND_ONCE and timeout options.
inline kern_return_t exchange(mach_port_t service,uint64_t nonce,mach_msg_timeout_t timeout=250) {
    ReceivePort reply;
    if(reply.get()==MACH_PORT_NULL)return KERN_RESOURCE_SHORTAGE;
    Message request;
    request.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND_ONCE);
    request.header.msgh_size=sizeof(request);
    request.header.msgh_remote_port=service;
    request.header.msgh_local_port=reply.get();
    request.header.msgh_id=requestID;request.nonce=nonce;
    auto status=mach_msg(&request.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(request),0,MACH_PORT_NULL,timeout,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS)return status;
    Received response;
    status=mach_msg(&response.message.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(response),reply.get(),timeout,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS)return status;
    const auto &h=response.message.header;
    const bool valid=!(h.msgh_bits&MACH_MSGH_BITS_COMPLEX) &&
        h.msgh_remote_port==MACH_PORT_NULL && h.msgh_size==sizeof(Message) &&
        h.msgh_id==replyID && response.message.nonce==nonce;
    // Dispose any unexpected transferred rights/descriptors, even on rejection.
    mach_msg_destroy(&response.message.header);
    return valid?KERN_SUCCESS:KERN_INVALID_VALUE;
}

inline kern_return_t exchangeAudio(mach_port_t service,uint64_t nonce,mach_msg_timeout_t timeout=250) {
    if(!nonce)return KERN_INVALID_ARGUMENT;
    ReceivePort reply;
    if(reply.get()==MACH_PORT_NULL)return KERN_RESOURCE_SHORTAGE;
    Message request;
    request.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND_ONCE);
    request.header.msgh_size=sizeof(request);
    request.header.msgh_remote_port=service;request.header.msgh_local_port=reply.get();
    request.header.msgh_id=audioRequestID;request.nonce=nonce;
    auto status=mach_msg(&request.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(request),0,MACH_PORT_NULL,timeout,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS)return status;
    AudioReceived response;
    status=mach_msg(&response.message.prefix.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(response),reply.get(),timeout,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS)return status;
    const bool valid=validateAudio(response.message,nonce);
    mach_msg_destroy(&response.message.prefix.header);
    return valid?KERN_SUCCESS:KERN_INVALID_VALUE;
}

// One bounded diagnostic request. The send-once reply right belongs to this
// request only. Receive errors never interpret a partial message.
inline kern_return_t serveOne(mach_port_t port,mach_msg_timeout_t timeout=250,bool corruptReply=false) {
    Received received;
    auto status=mach_msg(&received.message.header,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(received),port,timeout,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS)return status;
    auto &h=received.message.header;
    const bool valid=!(h.msgh_bits&MACH_MSGH_BITS_COMPLEX) && h.msgh_size==sizeof(Message) &&
        (h.msgh_id==requestID || h.msgh_id==audioRequestID) && h.msgh_remote_port!=MACH_PORT_NULL &&
        MACH_MSGH_BITS_REMOTE(h.msgh_bits)==MACH_MSG_TYPE_PORT_SEND_ONCE;
    if(!valid) {mach_msg_destroy(&h);return KERN_INVALID_VALUE;}
    if(h.msgh_id==audioRequestID) {
        AudioMessage reply;
        AudioFeedBatch batch;batch.epoch=received.message.nonce;batch.frames=512;
        for(uint32_t n=0;n<512;++n)batch.samples[n]=fixtureSample(n);
        size_t used=0;
        if(AudioFeedPacket::encode(batch,reply.packet,used)!=AudioFeedError::ok) {
            mach_msg_destroy(&h);return KERN_INVALID_VALUE;
        }
        auto &out=reply.prefix.header;
        out.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE,0);
        out.msgh_size=sizeof(reply);out.msgh_remote_port=h.msgh_remote_port;out.msgh_id=audioReplyID;
        reply.prefix.nonce=received.message.nonce;
        h.msgh_remote_port=MACH_PORT_NULL;mach_msg_destroy(&h);
        status=mach_msg(&out,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(reply),0,MACH_PORT_NULL,timeout,MACH_PORT_NULL);
        if(status!=KERN_SUCCESS)mach_msg_destroy(&out);
        return status;
    }
    Message reply;
    reply.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE,0);
    reply.header.msgh_size=sizeof(reply);reply.header.msgh_remote_port=h.msgh_remote_port;
    reply.header.msgh_id=replyID;reply.nonce=received.message.nonce+(corruptReply?1:0);
    // Transfer ownership into reply; never destroy the same right twice.
    h.msgh_remote_port=MACH_PORT_NULL;
    mach_msg_destroy(&h);
    status=mach_msg(&reply.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(reply),0,MACH_PORT_NULL,timeout,MACH_PORT_NULL);
    if(status!=KERN_SUCCESS)mach_msg_destroy(&reply.header);
    return status;
}
}
