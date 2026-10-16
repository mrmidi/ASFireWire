// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuplexMachChannel.hpp"
#include <chrono>
#include <memory>

namespace Pro40Audio {
// Control-thread capability exchange, independent of service registration and
// code-signing policy. The listener/lookup right is NOT an audio endpoint.
// A supervisor must authorize and retain the original worker before accept().
// The worker supplies its captured launch-parent token ONLY after managed entry.
// Both endpoints are private receive rights; only send references cross tasks.
struct DuplexLaunchBinding {
    uint64_t epoch=0,nonce=0;
    std::array<uint8_t,16> requestID{};
    bool valid()const noexcept {
        bool any=false;for(auto byte:requestID)any|=byte!=0;
        return epoch && nonce && any;
    }
};
namespace DuplexRendezvousWire {
inline constexpr mach_msg_id_t helloID=0x50344430,replyID=0x50344431;
using Bytes=std::array<uint8_t,40>;
struct Hello {mach_msg_header_t header{};Bytes bytes{};};
struct Reply {mach_msg_header_t header{};mach_msg_body_t body{};mach_msg_port_descriptor_t port{};Bytes bytes{};audit_token_t recipient=INVALID_AUDIT_TOKEN_VALUE;};
static_assert(sizeof(Hello)==sizeof(mach_msg_header_t)+40);
static_assert(sizeof(Reply)==sizeof(mach_msg_header_t)+sizeof(mach_msg_body_t)+sizeof(mach_msg_port_descriptor_t)+40+sizeof(audit_token_t));
inline Bytes encode(const DuplexLaunchBinding &binding)noexcept {
    Bytes b{};DuplexWire::put(b,0,4,2);DuplexWire::put(b,8,8,binding.epoch);DuplexWire::put(b,16,8,binding.nonce);
    std::copy(binding.requestID.begin(),binding.requestID.end(),b.begin()+24);return b;
}
inline bool sender(std::span<const std::byte> bytes,audit_token_t expected)noexcept {
    const audit_token_t invalid=INVALID_AUDIT_TOKEN_VALUE;
    const auto token=nativeAuditToken(bytes);
    return std::memcmp(&expected,&invalid,sizeof(expected)) && token && !std::memcmp(&*token,&expected,sizeof(expected));
}
inline bool validToken(audit_token_t token)noexcept {
    const audit_token_t invalid=INVALID_AUDIT_TOKEN_VALUE;
    return std::memcmp(&token,&invalid,sizeof(token))!=0;
}
inline std::optional<audit_token_t> selfToken()noexcept {
    // Installed SDK mach/task_info.h:242-244; query only this task. This is
    // identity provenance, not authorization to become another task's peer.
    audit_token_t token{};mach_msg_type_number_t count=TASK_AUDIT_TOKEN_COUNT;
    if(task_info(mach_task_self(),TASK_AUDIT_TOKEN,reinterpret_cast<task_info_t>(&token),&count)!=KERN_SUCCESS ||
       count!=TASK_AUDIT_TOKEN_COUNT || !validToken(token))return {};
    return token;
}
inline auto channel(NativeMachPort &receive,NativeMachPort &peer,audit_token_t expected,DuplexLaunchBinding binding) {
    // Allocate before relinquishing either reference. Constructor adoption is
    // nonthrowing; an allocation failure leaves both rights with their owners.
    auto result=std::make_unique<DuplexMachChannel>(receive.get(),peer.get(),expected,binding.epoch,binding.nonce);
    (void)receive.release();(void)peer.release();return result;
}
}
// Owns a fresh worker endpoint while waiting. start() retains the lookup SEND
// reference; caller can close its lookup immediately. poll() never blocks.
class DuplexMachConnector final {
public:
    using Clock=std::chrono::steady_clock;
    enum class State {waiting,connected,failed};
private:
    NativeMachPort listener_{MACH_PORT_RIGHT_SEND},receive_{MACH_PORT_RIGHT_RECEIVE};
    DuplexLaunchBinding binding_;
    audit_token_t parent_{};
    Clock::time_point deadline_{};
    bool started_=false,sent_=false;
    State state_=State::waiting;
    std::unique_ptr<DuplexMachChannel> channel_;
    State fail()noexcept{state_=State::failed;(void)listener_.close();(void)receive_.close();return state_;}
public:
    [[nodiscard]] bool start(mach_port_t listener,audit_token_t parent,DuplexLaunchBinding binding,Clock::time_point deadline) {
        if(started_)return false;started_=true;
        binding_=binding;parent_=parent;deadline_=deadline;
        const audit_token_t invalid=INVALID_AUDIT_TOKEN_VALUE;
        if(!binding.valid() || !std::memcmp(&parent,&invalid,sizeof(parent)) || deadline<=Clock::now() ||
           listener_.retainSend(listener)!=KERN_SUCCESS || receive_.allocate()!=KERN_SUCCESS){fail();return false;}
        return true;
    }
    State poll(bool stopped=false) {
        using namespace DuplexRendezvousWire;
        if(state_!=State::waiting)return state_;
        if(!started_ || stopped || Clock::now()>=deadline_)return fail();
        if(!sent_) {
            Hello h;h.bytes=encode(binding_);h.header.msgh_id=helloID;h.header.msgh_size=sizeof(h);
            h.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,MACH_MSG_TYPE_MAKE_SEND);
            h.header.msgh_remote_port=listener_.get();h.header.msgh_local_port=receive_.get();
            const auto r=mach_msg(&h.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(h),0,MACH_PORT_NULL,0,MACH_PORT_NULL);
            if(r==MACH_SEND_TIMED_OUT)return state_;
            if(r!=KERN_SUCCESS || listener_.close()!=KERN_SUCCESS)return fail();sent_=true;
        }
        NativeMachReceived<512> message;
        if(!message.poll(receive_.get(),nativeAuditReceiveOptions))
            return message.status()==MACH_RCV_TIMED_OUT?state_:fail();
        Reply r;std::memcpy(&r,message.bytes().data(),sizeof(r));
        if(r.header.msgh_id!=replyID || r.header.msgh_size!=sizeof(r) ||
           !(r.header.msgh_bits&MACH_MSGH_BITS_COMPLEX) || r.header.msgh_remote_port!=MACH_PORT_NULL ||
           r.header.msgh_local_port!=receive_.get() || r.header.msgh_voucher_port!=MACH_PORT_NULL ||
           r.body.msgh_descriptor_count!=1 || r.port.type!=MACH_MSG_PORT_DESCRIPTOR ||
           r.port.disposition!=MACH_MSG_TYPE_PORT_SEND || !MACH_PORT_VALID(r.port.name) ||
           r.bytes!=encode(binding_) || !validToken(r.recipient) || !sender(message.bytes(),parent_) || Clock::now()>=deadline_)return fail();
        NativeMachPort peer(MACH_PORT_RIGHT_SEND);
        if(peer.retainSend(r.port.name)!=KERN_SUCCESS)return fail();
        // Only the already captured launch parent can authorize this recipient.
        // Subsequent audio requires that EXACT full token, even when the parent
        // delegates to a separately authenticated CoreAudio helper. The worker
        // never learns an expected identity from an unauthenticated audio peer.
        channel_=DuplexRendezvousWire::channel(receive_,peer,r.recipient,binding_);
        state_=State::connected;return state_;
    }
    [[nodiscard]] std::unique_ptr<DuplexMachChannel> takeChannel()noexcept{return std::move(channel_);}
};

// A staged capability, not an audio session or proof of receiver authorization.
// Controller keeps original worker ownership. It must authenticate/admit the
// receiver, send it workerPort()+worker identity+binding, and obtain its control
// acknowledgement BEFORE commit(). Until then the worker cannot enter audio.
// Cancellation/destruction only releases this send reference; the supervisor
// still has to stop/join its retained child. Never move a receive right.
class DuplexEndpointGrant final {
    NativeMachPort worker_{MACH_PORT_RIGHT_SEND};
    const DuplexLaunchBinding binding_;
    const DuplexMachConnector::Clock::time_point deadline_;
    bool attempted_=false,committed_=false;
    friend class DuplexMachAcceptor;
    DuplexEndpointGrant(NativeMachPort worker,DuplexLaunchBinding binding,DuplexMachConnector::Clock::time_point deadline)
        :worker_(std::move(worker)),binding_(binding),deadline_(deadline){}
public:
    mach_port_t workerPort()const noexcept{return worker_.get();} // Borrow only while this grant lives.
    // Explicit retirement lets a delegation owner include its retained worker
    // SEND reference in local cleanup evidence, also before commit/cancellation.
    [[nodiscard]] bool close()noexcept {attempted_=true;return worker_.close()==KERN_SUCCESS;}
    [[nodiscard]] bool commit(mach_port_t authorizedReceiver,audit_token_t recipient,bool stopped=false)noexcept {
        using namespace DuplexRendezvousWire;
        if(attempted_)return false;attempted_=true;
        if(stopped || !validToken(recipient) || !MACH_PORT_VALID(authorizedReceiver) ||
           DuplexMachConnector::Clock::now()>=deadline_)return false;
        mach_port_type_t type=0;
        if(mach_port_type(mach_task_self(),authorizedReceiver,&type)!=KERN_SUCCESS || !(type&MACH_PORT_TYPE_SEND))return false;
        // A dead name can otherwise be copied successfully as a descriptor.
        // Death after this check remains possible; the worker rejects a dead
        // received port. Commit means delivery, never peer/process liveness.
        Reply r;r.bytes=encode(binding_);r.recipient=recipient;r.header.msgh_id=replyID;r.header.msgh_size=sizeof(r);
        r.header.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,0)|MACH_MSGH_BITS_COMPLEX;
        r.header.msgh_remote_port=worker_.get();r.body.msgh_descriptor_count=1;
        r.port.name=authorizedReceiver;r.port.disposition=MACH_MSG_TYPE_COPY_SEND;r.port.type=MACH_MSG_PORT_DESCRIPTOR;
        // Both references remain owned on timeout/failure. No capability retry.
        committed_=mach_msg(&r.header,MACH_SEND_MSG|MACH_SEND_TIMEOUT,sizeof(r),0,MACH_PORT_NULL,0,MACH_PORT_NULL)==KERN_SUCCESS;
        return committed_;
    }
    [[nodiscard]] mach_port_t takeWorkerPort()noexcept{return committed_?worker_.release():MACH_PORT_NULL;}
};

// One-shot accept on an independently registered control service. All admission
// checks precede giving the worker a private send right. A failed send is final;
// no silent re-admission, partial channel or retry that duplicates capabilities.
class DuplexMachAcceptor final {
    bool attempted_=false;
public:
    template<size_t N> [[nodiscard]] std::unique_ptr<DuplexEndpointGrant> prepare(
        const NativeMachReceived<N> &message,mach_port_t listener,audit_token_t authorizedWorker,
        DuplexLaunchBinding binding,DuplexMachConnector::Clock::time_point deadline,bool stopped=false) {
        using namespace DuplexRendezvousWire;
        if(attempted_)return {};attempted_=true;
        if(!message.received() || message.bytes().size()<sizeof(Hello) || !binding.valid() ||
           stopped || DuplexMachConnector::Clock::now()>=deadline)return {};
        Hello h;std::memcpy(&h,message.bytes().data(),sizeof(h));
        if(h.header.msgh_id!=helloID || h.header.msgh_size!=sizeof(h) ||
           (h.header.msgh_bits&MACH_MSGH_BITS_COMPLEX) || h.header.msgh_voucher_port!=MACH_PORT_NULL ||
           h.header.msgh_local_port!=listener || !MACH_PORT_VALID(h.header.msgh_remote_port) ||
           MACH_MSGH_BITS_REMOTE(h.header.msgh_bits)!=MACH_MSG_TYPE_PORT_SEND ||
           h.bytes!=encode(binding) || !sender(message.bytes(),authorizedWorker))return {};
        NativeMachPort peer(MACH_PORT_RIGHT_SEND);
        if(peer.retainSend(h.header.msgh_remote_port)!=KERN_SUCCESS)return {};
        return std::unique_ptr<DuplexEndpointGrant>(new DuplexEndpointGrant(std::move(peer),binding,deadline));
    }
    template<size_t N> [[nodiscard]] std::unique_ptr<DuplexMachChannel> accept(
        const NativeMachReceived<N> &message,mach_port_t listener,audit_token_t authorizedWorker,
        DuplexLaunchBinding binding,DuplexMachConnector::Clock::time_point deadline,bool stopped=false) {
        auto grant=prepare(message,listener,authorizedWorker,binding,deadline,stopped);
        auto self=DuplexRendezvousWire::selfToken();
        if(!grant || !self)return {};
        NativeMachPort receive(MACH_PORT_RIGHT_RECEIVE),send(MACH_PORT_RIGHT_SEND),peer(MACH_PORT_RIGHT_SEND);
        if(receive.allocate()!=KERN_SUCCESS ||
           mach_port_insert_right(mach_task_self(),receive.get(),receive.get(),MACH_MSG_TYPE_MAKE_SEND)!=KERN_SUCCESS)return {};
        if(send.adopt(receive.get())!=KERN_SUCCESS)return {};
        if(!grant->commit(send.get(),*self,stopped))return {};
        if(peer.adopt(grant->takeWorkerPort())!=KERN_SUCCESS)return {};
        return DuplexRendezvousWire::channel(receive,peer,authorizedWorker,binding);
    }
};
}
