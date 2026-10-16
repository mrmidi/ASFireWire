// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFeedMachTransport.hpp"
namespace Pro40Audio {
// Adopts exactly one pre-established send right. One non-RT sender thread owns
// all calls and destruction. No bootstrap/admission, retries, hardware or HAL.
// COPY_SEND retains our right until close(); timeout is per message, not a hard
// scheduling bound. Failed sends are terminal, never replayed ambiguously.
template<size_t Channels> class NativeFeedMachSenderFor {
    using Batch=AudioFeedBatchFor<Channels>;
    using Packet=AudioFeedPacketFor<Channels>;
    using Message=FeedMachMessageFor<Channels>;
public:
    enum class State {active,ended,failed,closed};
    NativeFeedMachSenderFor(mach_port_t port,uint64_t epoch,mach_msg_timeout_t timeout)
        :port_(port),epoch_(epoch),timeout_(timeout){if(!MACH_PORT_VALID(port)||!epoch||!timeout)state_=State::failed;}
    ~NativeFeedMachSenderFor(){close();}
    NativeFeedMachSenderFor(const NativeFeedMachSenderFor &)=delete;
    NativeFeedMachSenderFor &operator=(const NativeFeedMachSenderFor &)=delete;
    State state()const noexcept{return state_;}
    kern_return_t lastSendStatus()const noexcept{return sendStatus_;}
    uint64_t acceptedFrames()const noexcept{return next_;}
    bool send(const Batch &batch) noexcept {
        if(state_!=State::active||batch.epoch!=epoch_||batch.firstFrame!=next_)return fail();
        Message message;size_t used=0;
        if(Packet::encode(batch,message.packet,used)!=AudioFeedError::ok)return fail();
        message.header.msgh_id=feedMessageID;
        message.header.msgh_size=mach_msg_size_t(sizeof(mach_msg_header_t)+used);
        if(!emit(message))return false;
        next_+=batch.frames;return true;
    }
    bool finish(uint64_t epoch,uint64_t end) noexcept {
        if(state_!=State::active||epoch!=epoch_||end!=next_)return fail();
        Message message;
        if(!AudioFeedControl{AudioFeedControl::Kind::end,epoch,end}.encode(
            std::span(message.packet).first(AudioFeedControl::size)))return fail();
        message.header.msgh_id=feedControlID;
        message.header.msgh_size=sizeof(mach_msg_header_t)+AudioFeedControl::size;
        if(!emit(message))return false;
        state_=State::ended;return true;
    }
    bool close() noexcept {
        if(MACH_PORT_VALID(port_)){
            const auto port=port_;port_=MACH_PORT_NULL;
            cleanupOK_=mach_port_deallocate(mach_task_self(),port)==KERN_SUCCESS;
        }
        state_=State::closed;return cleanupOK_;
    }
private:
    bool fail() noexcept {state_=State::failed;return false;}
    bool emit(Message &message) noexcept {
        auto &h=message.header;h.msgh_remote_port=port_;
        h.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,0);
        sendStatus_=mach_msg(&h,MACH_SEND_MSG|MACH_SEND_TIMEOUT,h.msgh_size,0,MACH_PORT_NULL,timeout_,MACH_PORT_NULL);
        return sendStatus_==KERN_SUCCESS?true:fail();
    }
    mach_port_t port_;uint64_t epoch_,next_=0;mach_msg_timeout_t timeout_;
    State state_=State::active;kern_return_t sendStatus_=KERN_SUCCESS;bool cleanupOK_=true;
};
using NativeFeedMachSender=NativeFeedMachSenderFor<1>;
}
