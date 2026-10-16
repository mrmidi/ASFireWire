// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFeedWorker.hpp"
#include <mach/mach.h>
#include <cstring>

namespace Pro40Audio {
// Private diagnostic feed, not FireWire and not the initialization ping ABI.
inline constexpr mach_msg_id_t feedMessageID=0x50344001,feedDisconnectID=0x50344002;
inline constexpr mach_msg_id_t feedControlID=0x50344003;
// A positive sub-millisecond budget still permits one NONBLOCKING receive.
// Returning idle without polling here starves a worker whose poll quantum is1ms:
// call overhead rounds every remaining budget down to zero.
inline std::optional<mach_msg_timeout_t> feedMachTimeout(std::chrono::steady_clock::duration remaining) noexcept {
    if(remaining<=std::chrono::steady_clock::duration::zero())return std::nullopt;
    const auto millis=std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    return mach_msg_timeout_t(std::min<int64_t>(millis,UINT32_MAX));
}
template<size_t Channels> struct FeedMachMessageFor {
    mach_msg_header_t header{};
    std::array<uint8_t,AudioFeedPacketFor<Channels>::maxBytes> packet{};
};
using FeedMachMessage=FeedMachMessageFor<1>;
static_assert(sizeof(FeedMachMessage)==sizeof(mach_msg_header_t)+AudioFeedPacket::maxBytes);
// Adopts exactly one pre-established receive right. Admission/peer identity and
// bootstrap lookup belong outside poll. One owner, move before run only.
template<size_t Channels> class AudioFeedMachTransportFor {
    using Packet=AudioFeedPacketFor<Channels>;
    using Message=FeedMachMessageFor<Channels>;
    static_assert(sizeof(Message)==sizeof(mach_msg_header_t)+Packet::maxBytes);
    mach_port_t receive_=MACH_PORT_NULL;
    bool cleanupOK_=true;
public:
    explicit AudioFeedMachTransportFor(mach_port_t receive):receive_(receive) {}
    AudioFeedMachTransportFor(const AudioFeedMachTransportFor &)=delete;
    AudioFeedMachTransportFor &operator=(const AudioFeedMachTransportFor &)=delete;
    AudioFeedMachTransportFor(AudioFeedMachTransportFor &&other) noexcept:
        receive_(std::exchange(other.receive_,MACH_PORT_NULL)),cleanupOK_(other.cleanupOK_) {}
    ~AudioFeedMachTransportFor() {close();}
    bool close() noexcept {
        if(receive_!=MACH_PORT_NULL) {
            const auto port=std::exchange(receive_,MACH_PORT_NULL);
            cleanupOK_=mach_port_mod_refs(mach_task_self(),port,MACH_PORT_RIGHT_RECEIVE,-1)==KERN_SUCCESS;
        }
        return cleanupOK_;
    }
    FeedPoll poll(std::span<uint8_t> output,size_t &used,std::chrono::steady_clock::time_point deadline) noexcept {
        used=0;
        if(receive_==MACH_PORT_NULL)return FeedPoll::disconnected;
        const auto timeout=feedMachTimeout(deadline-std::chrono::steady_clock::now());
        // Round DOWN to the Mach millisecond API, never stretch the supplied
        // deadline. Scheduling can still delay return; no hard realtime claim.
        if(!timeout)return FeedPoll::idle;
        struct Received {Message message{};mach_msg_max_trailer_t trailer{};} received;
        auto &h=received.message.header;
        // No MACH_RCV_LARGE: oversized messages are rejected, never allocated.
        const auto result=mach_msg(&h,MACH_RCV_MSG|MACH_RCV_TIMEOUT,0,sizeof(received),receive_,*timeout,MACH_PORT_NULL);
        if(result==MACH_RCV_TIMED_OUT)return FeedPoll::idle;
        if(result==MACH_RCV_PORT_DIED || result==MACH_RCV_INVALID_NAME)return FeedPoll::disconnected;
        if(result!=KERN_SUCCESS)return FeedPoll::error;
        FeedPoll answer=FeedPoll::error;
        if(!(h.msgh_bits&MACH_MSGH_BITS_COMPLEX) && h.msgh_remote_port==MACH_PORT_NULL) {
            if(h.msgh_id==feedDisconnectID && h.msgh_size==sizeof(h))answer=FeedPoll::disconnected;
            else if(h.msgh_id==feedControlID && h.msgh_size==sizeof(h)+AudioFeedControl::size && output.size()>=AudioFeedControl::size) {
                std::memcpy(output.data(),received.message.packet.data(),AudioFeedControl::size);
                used=AudioFeedControl::size;answer=FeedPoll::message;
            }
            else if(h.msgh_id==feedMessageID && h.msgh_size>=sizeof(h)+Packet::headerBytes &&
                    h.msgh_size<=sizeof(Message)) {
                const size_t count=h.msgh_size-sizeof(h);
                if(output.size()>=count) {
                    std::memcpy(output.data(),received.message.packet.data(),count);
                    used=count;answer=FeedPoll::message;
                }
            }
        }
        // Includes unexpected reply rights/descriptors on rejected envelopes.
        mach_msg_destroy(&h);
        return answer;
    }
};
using AudioFeedMachTransport=AudioFeedMachTransportFor<1>;
}
