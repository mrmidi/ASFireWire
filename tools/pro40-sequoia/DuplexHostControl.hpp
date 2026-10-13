// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "DuplexMachRendezvous.hpp"
#include "DuplexProvisionTransport.hpp"
#include "NativePeerIdentity.hpp"
namespace Pro40Audio {
// Non-RT control channel for a SINGLE delegated session. It carries no PCM.
// Expected peer/token and nonce come from authenticated provisioning, never
// from a control message. Each direction has an independent strict sequence.
class DuplexHostControl final {
public:
    enum class Kind:uint32_t {ready=1,committed=2,active=3,released=4,cancel=5};
    enum class Result {ok,idle,full,invalid,transport,closed};
    struct Event {Kind kind=Kind::cancel;uint32_t status=0;};
private:
    struct Message {mach_msg_header_t header{};std::array<uint8_t,32> bytes{};};
    NativeMachPort receive_,peer_;
    const audit_token_t token_;
    const uint64_t nonce_;
    uint64_t sent_=0,received_=0;
    bool healthy_=true,closed_=false,cleanupOK_=true;
    static bool valid(Kind k,uint32_t status)noexcept {
        return uint32_t(k)>=1 && uint32_t(k)<=5 && (k==Kind::released?status<=7:status==0);
    }
public:
    DuplexHostControl(NativeMachPort receive,NativeMachPort peer,audit_token_t token,uint64_t nonce)
        :receive_(std::move(receive)),peer_(std::move(peer)),token_(token),nonce_(nonce) {
        healthy_=MACH_PORT_VALID(receive_.get()) && MACH_PORT_VALID(peer_.get()) && DuplexRendezvousWire::validToken(token) && nonce;
    }
    DuplexHostControl(const DuplexHostControl &)=delete;
    ~DuplexHostControl(){(void)close();}
    [[nodiscard]] Result send(Kind kind,uint32_t status=0)noexcept {
        if(closed_)return Result::closed;
        if(!healthy_ || !valid(kind,status) || sent_==UINT64_MAX){healthy_=false;return Result::invalid;}
        Message message;auto &b=message.bytes;
        DuplexWire::put(b,0,4,1);DuplexWire::put(b,4,4,uint32_t(kind));DuplexWire::put(b,8,4,status);
        DuplexWire::put(b,16,8,nonce_);DuplexWire::put(b,24,8,sent_+1);
        ProvisionTransport::outgoing(message.header,peer_.get(),0x50344462,sizeof(message));
        const auto result=ProvisionTransport::send(message.header,0);
        if(result==MACH_SEND_TIMED_OUT)return Result::full;
        if(result!=KERN_SUCCESS){healthy_=false;return Result::transport;}
        ++sent_;return Result::ok;
    }
    [[nodiscard]] Result poll(Event &event)noexcept {
        if(closed_)return Result::closed;if(!healthy_)return Result::invalid;
        NativeMachReceived<256> received;
        if(!received.poll(receive_.get(),nativeAuditReceiveOptions)) {
            if(received.status()==MACH_RCV_TIMED_OUT)return Result::idle;
            healthy_=false;return Result::transport;
        }
        Message message;std::memcpy(&message,received.bytes().data(),sizeof(message));
        const auto &h=message.header;const auto &b=message.bytes;
        const auto kind=Kind(DuplexWire::get(b,4,4));const auto status=uint32_t(DuplexWire::get(b,8,4));
        if(h.msgh_size!=sizeof(message) || h.msgh_id!=0x50344462 || (h.msgh_bits&MACH_MSGH_BITS_COMPLEX) ||
           h.msgh_local_port!=receive_.get() || h.msgh_remote_port!=MACH_PORT_NULL || h.msgh_voucher_port!=MACH_PORT_NULL ||
           !DuplexRendezvousWire::sender(received.bytes(),token_) || DuplexWire::get(b,0,4)!=1 || DuplexWire::get(b,12,4) ||
           DuplexWire::get(b,16,8)!=nonce_ || received_==UINT64_MAX || DuplexWire::get(b,24,8)!=received_+1 || !valid(kind,status)) {
            healthy_=false;return Result::invalid;
        }
        ++received_;event={kind,status};return Result::ok;
    }
    [[nodiscard]] bool close()noexcept {
        closed_=true;const auto receive=receive_.close(),peer=peer_.close();
        cleanupOK_=receive==KERN_SUCCESS && peer==KERN_SUCCESS && cleanupOK_;return cleanupOK_;
    }
};

namespace DuplexHostWire {
inline constexpr mach_msg_id_t startID=0x50344460,offerID=0x50344461;
struct Values {
    uint64_t nonce=0,seed=0;uint32_t numer=0,denom=0;
    bool valid()const noexcept{return nonce && seed && numer && denom;}
    bool operator==(const Values &)const=default;
};
struct Start {mach_msg_header_t header{};mach_msg_body_t body{};mach_msg_port_descriptor_t audio{},completion{};std::array<uint8_t,32> bytes{};};
struct Offer {mach_msg_header_t header{};mach_msg_body_t body{};mach_msg_port_descriptor_t worker{},control{};std::array<uint8_t,112> bytes{};};
static_assert(sizeof(Start)==84 && sizeof(Offer)==164);
// Controller owns these SEND references until its delegation/cleanup owner has
// joined. Accepting this request authenticates the helper; it does not release a
// worker or admit audio. The registry endpoint is intentionally only a listener.
struct Request {
    NativeMachPort reply{MACH_PORT_RIGHT_SEND},audio{MACH_PORT_RIGHT_SEND},completion{MACH_PORT_RIGHT_SEND};
    audit_token_t helper=INVALID_AUDIT_TOKEN_VALUE;
    Values values;
};
inline kern_return_t start(mach_port_t controller,mach_port_t replyReceive,mach_port_t audioReceive,mach_port_t completionReceive,Values values)noexcept {
    if(!values.valid() || !MACH_PORT_VALID(controller) || !MACH_PORT_VALID(replyReceive) || !MACH_PORT_VALID(audioReceive) ||
       !MACH_PORT_VALID(completionReceive) || replyReceive==audioReceive || replyReceive==completionReceive || audioReceive==completionReceive)return KERN_INVALID_ARGUMENT;
    Start request;ProvisionTransport::outgoing(request.header,controller,startID,sizeof(request),true);
    request.header.msgh_local_port=replyReceive;request.header.msgh_bits|=MACH_MSGH_BITS(0,MACH_MSG_TYPE_MAKE_SEND);
    request.body.msgh_descriptor_count=2;request.audio.name=audioReceive;
    request.audio.type=MACH_MSG_PORT_DESCRIPTOR;request.audio.disposition=MACH_MSG_TYPE_MAKE_SEND;
    request.completion.name=completionReceive;request.completion.type=MACH_MSG_PORT_DESCRIPTOR;request.completion.disposition=MACH_MSG_TYPE_MAKE_SEND;
    DuplexWire::put(request.bytes,0,4,1);DuplexWire::put(request.bytes,8,8,values.nonce);
    DuplexWire::put(request.bytes,16,8,values.seed);DuplexWire::put(request.bytes,24,4,values.numer);DuplexWire::put(request.bytes,28,4,values.denom);
    return ProvisionTransport::send(request.header,0);
}
template<size_t N> inline std::unique_ptr<Request> accept(const NativeMachReceived<N> &message,mach_port_t listener,
                                                       SecRequirementRef expectedHelper,uint32_t numer,uint32_t denom) {
    if(!message.received() || message.bytes().size()<sizeof(Start) || !expectedHelper || !numer || !denom)return {};
    Start start;std::memcpy(&start,message.bytes().data(),sizeof(start));const auto &h=start.header;const auto &b=start.bytes;
    auto token=nativeAuditToken(message.bytes());
    const Values values{DuplexWire::get(b,8,8),DuplexWire::get(b,16,8),uint32_t(DuplexWire::get(b,24,4)),uint32_t(DuplexWire::get(b,28,4))};
    if(h.msgh_id!=startID || h.msgh_size!=sizeof(start) || !(h.msgh_bits&MACH_MSGH_BITS_COMPLEX) ||
       !ProvisionTransport::requestEnvelope(h,listener) || !token || !values.valid() || values.numer!=numer || values.denom!=denom ||
       DuplexWire::get(b,0,4)!=1 || DuplexWire::get(b,4,4) || start.body.msgh_descriptor_count!=2 ||
       start.audio.type!=MACH_MSG_PORT_DESCRIPTOR || start.audio.disposition!=MACH_MSG_TYPE_PORT_SEND || !MACH_PORT_VALID(start.audio.name) ||
       start.completion.type!=MACH_MSG_PORT_DESCRIPTOR || start.completion.disposition!=MACH_MSG_TYPE_PORT_SEND || !MACH_PORT_VALID(start.completion.name) ||
       start.audio.name==start.completion.name || start.audio.name==h.msgh_remote_port || start.completion.name==h.msgh_remote_port ||
       verifyNativePeerIdentity(*token,expectedHelper)!=errSecSuccess)return {};
    auto result=std::make_unique<Request>();
    if(result->reply.retainSend(h.msgh_remote_port)!=KERN_SUCCESS || result->audio.retainSend(start.audio.name)!=KERN_SUCCESS ||
       result->completion.retainSend(start.completion.name)!=KERN_SUCCESS)return {};
    result->values=values;result->helper=*token;return result;
}
// The controller must retain the original worker grant and its own private
// control receive right until READY. Only then may it commit the worker grant.
inline kern_return_t offer(const Request &request,mach_port_t worker,mach_port_t controlReceive,
                          audit_token_t workerToken,DuplexLaunchBinding binding,SourceFrameClock::Identity identity)noexcept {
    if(!request.values.valid() || !binding.valid() || !DuplexRendezvousWire::validToken(workerToken) ||
       identity.epoch!=binding.epoch || identity.seed!=request.values.seed || identity.numer!=request.values.numer || identity.denom!=request.values.denom ||
       !MACH_PORT_VALID(worker) || !MACH_PORT_VALID(controlReceive))return KERN_INVALID_ARGUMENT;
    Offer message;ProvisionTransport::outgoing(message.header,request.reply.get(),offerID,sizeof(message),true);
    message.body.msgh_descriptor_count=2;message.worker.name=worker;message.worker.type=MACH_MSG_PORT_DESCRIPTOR;message.worker.disposition=MACH_MSG_TYPE_COPY_SEND;
    message.control.name=controlReceive;message.control.type=MACH_MSG_PORT_DESCRIPTOR;message.control.disposition=MACH_MSG_TYPE_MAKE_SEND;
    auto &b=message.bytes;DuplexWire::put(b,0,4,1);DuplexWire::put(b,8,8,request.values.nonce);DuplexWire::put(b,16,8,identity.seed);
    DuplexWire::put(b,24,4,identity.generation);DuplexWire::put(b,28,4,identity.numer);DuplexWire::put(b,32,4,identity.denom);
    const auto encoded=DuplexRendezvousWire::encode(binding);std::copy(encoded.begin(),encoded.end(),b.begin()+40);
    std::memcpy(b.data()+80,&workerToken,sizeof(workerToken));return ProvisionTransport::send(message.header,0);
}
struct Offered {
    NativeMachPort worker{MACH_PORT_RIGHT_SEND},control{MACH_PORT_RIGHT_SEND};
    audit_token_t token=INVALID_AUDIT_TOKEN_VALUE;
    DuplexLaunchBinding binding;
    SourceFrameClock::Identity identity;
};
template<size_t N> inline std::unique_ptr<Offered> receiveOffer(const NativeMachReceived<N> &received,mach_port_t replyReceive,
                                                            audit_token_t controller,Values requested) {
    if(!received.received() || received.bytes().size()<sizeof(Offer) || !requested.valid())return {};
    Offer message;std::memcpy(&message,received.bytes().data(),sizeof(message));const auto &h=message.header;const auto &b=message.bytes;
    if(h.msgh_size!=sizeof(message) || h.msgh_id!=offerID || !(h.msgh_bits&MACH_MSGH_BITS_COMPLEX) ||
       h.msgh_local_port!=replyReceive || h.msgh_remote_port!=MACH_PORT_NULL || h.msgh_voucher_port!=MACH_PORT_NULL ||
       !DuplexRendezvousWire::sender(received.bytes(),controller) || message.body.msgh_descriptor_count!=2 ||
       message.worker.type!=MACH_MSG_PORT_DESCRIPTOR || message.worker.disposition!=MACH_MSG_TYPE_PORT_SEND || !MACH_PORT_VALID(message.worker.name) ||
       message.control.type!=MACH_MSG_PORT_DESCRIPTOR || message.control.disposition!=MACH_MSG_TYPE_PORT_SEND || !MACH_PORT_VALID(message.control.name) ||
       DuplexWire::get(b,0,4)!=1 || DuplexWire::get(b,4,4) || DuplexWire::get(b,36,4) ||
       Values{DuplexWire::get(b,8,8),DuplexWire::get(b,16,8),uint32_t(DuplexWire::get(b,28,4)),uint32_t(DuplexWire::get(b,32,4))}!=requested)return {};
    auto result=std::make_unique<Offered>();
    result->binding.epoch=DuplexWire::get(b,48,8);result->binding.nonce=DuplexWire::get(b,56,8);
    std::copy(b.begin()+64,b.begin()+80,result->binding.requestID.begin());
    const auto encoded=DuplexRendezvousWire::encode(result->binding);
    if(!result->binding.valid() || !std::equal(encoded.begin(),encoded.end(),b.begin()+40))return {};
    std::memcpy(&result->token,b.data()+80,sizeof(result->token));if(!DuplexRendezvousWire::validToken(result->token))return {};
    result->identity={result->binding.epoch,requested.seed,uint32_t(DuplexWire::get(b,24,4)),requested.numer,requested.denom};
    if(result->worker.retainSend(message.worker.name)!=KERN_SUCCESS || result->control.retainSend(message.control.name)!=KERN_SUCCESS)return {};
    return result;
}
// Separate one-shot receipt, sent ONLY after retained child, verifier, watchdog
// and delegation users have joined. No control-channel sequence survives its
// owner's close. A positive native STOPPED alone is not this receipt.
struct Receipt {mach_msg_header_t header{};std::array<uint8_t,16> bytes{};};
inline kern_return_t joined(mach_port_t completion,uint64_t nonce,bool clean)noexcept {
    if(!MACH_PORT_VALID(completion) || !nonce)return KERN_INVALID_ARGUMENT;
    Receipt message;ProvisionTransport::outgoing(message.header,completion,0x50344463,sizeof(message));
    DuplexWire::put(message.bytes,0,4,1);DuplexWire::put(message.bytes,4,4,clean);DuplexWire::put(message.bytes,8,8,nonce);
    return ProvisionTransport::send(message.header,0);
}
template<size_t N> inline std::optional<bool> receipt(const NativeMachReceived<N> &received,mach_port_t completion,
                                                    audit_token_t controller,uint64_t nonce)noexcept {
    if(!nonce || !received.received() || received.bytes().size()<sizeof(Receipt))return {};
    Receipt message;std::memcpy(&message,received.bytes().data(),sizeof(message));const auto &h=message.header;const auto &b=message.bytes;
    if(h.msgh_id!=0x50344463 || h.msgh_size!=sizeof(message) || (h.msgh_bits&MACH_MSGH_BITS_COMPLEX) ||
       h.msgh_local_port!=completion || h.msgh_remote_port!=MACH_PORT_NULL || h.msgh_voucher_port!=MACH_PORT_NULL ||
       !DuplexRendezvousWire::sender(received.bytes(),controller) || DuplexWire::get(b,0,4)!=1 || DuplexWire::get(b,4,4)>1 || DuplexWire::get(b,8,8)!=nonce)return {};
    return DuplexWire::get(b,4,4)==1;
}
}
}
