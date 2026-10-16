// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeAuditTrailer.hpp"
#include "NativeMachOwnership.hpp"
#include "NativeDuplexPacket.hpp"
#include "AudioFeedPacket.hpp"
namespace Pro40Audio {
// Non-RT, single-owner, pre-connected channel. The supervisor supplies owned
// rights and an independently authorized kernel audit token, NEVER a token
// learned from these packets. No lookup, signing check or process launch here.
// Every receive checks the full opaque token (not just PID), epoch, nonce and
// strictly ordered sequence. Kernel trailers establish sender provenance only;
// authorization/provisioning of the expected token remains the supervisor's job.
class DuplexMachChannel {
public:
    enum class Kind:uint32_t {start=1,input=2,clock=3,output=4,activate=5,stop=6,stopped=7,credit=8};
    enum class Result {ok,idle,full,invalid,transport,closed};
    static constexpr mach_msg_id_t messageID=0x50344420;
    static constexpr size_t prefix=32,maxPayload=AudioFeedPacketFor<8>::maxBytes;
    struct Packet {
        Kind kind=Kind::stop;size_t used=0;
        std::array<uint8_t,maxPayload> bytes{};
        std::span<const uint8_t> payload() const noexcept {return std::span(bytes).first(used);}
    };
private:
    struct Message {mach_msg_header_t header{};std::array<uint8_t,prefix+maxPayload> bytes{};};
    static_assert(sizeof(Message)==sizeof(mach_msg_header_t)+prefix+maxPayload);
    NativeMachPort receive_{MACH_PORT_RIGHT_RECEIVE},peer_{MACH_PORT_RIGHT_SEND};
    const audit_token_t expected_;
    const uint64_t epoch_,nonce_;
    uint64_t sent_=0,received_=0;
    bool valid_=true,closed_=false,cleanupOK_=true;
    Result fail(Result r) noexcept {valid_=false;return r;}
public:
    DuplexMachChannel(mach_port_t receive,mach_port_t peer,audit_token_t expected,uint64_t epoch,uint64_t nonce)
        :expected_(expected),epoch_(epoch),nonce_(nonce) {
        const auto a=receive_.adopt(receive),b=peer_.adopt(peer);
        const audit_token_t invalid=INVALID_AUDIT_TOKEN_VALUE;
        valid_=a==KERN_SUCCESS && b==KERN_SUCCESS && epoch && nonce &&
            std::memcmp(&expected_,&invalid,sizeof(expected_))!=0;
    }
    DuplexMachChannel(const DuplexMachChannel &)=delete;
    ~DuplexMachChannel(){close();}
    bool healthy() const noexcept {return valid_ && !closed_;}
    uint64_t epoch() const noexcept {return epoch_;}
    uint64_t nonce() const noexcept {return nonce_;}
    Result send(Kind kind,std::span<const uint8_t> payload={}) noexcept {
        if(closed_)return Result::closed;
        if(!valid_ || uint32_t(kind)<1 || uint32_t(kind)>8 || payload.size()>maxPayload ||
           payload.size()%4 || sent_==UINT64_MAX)return fail(Result::invalid);
        Message m;auto &h=m.header;
        h.msgh_bits=MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,0);h.msgh_remote_port=peer_.get();
        h.msgh_id=messageID;h.msgh_size=sizeof(h)+prefix+payload.size();
        DuplexWire::put(m.bytes,0,4,uint32_t(kind));DuplexWire::put(m.bytes,4,4,payload.size());
        DuplexWire::put(m.bytes,8,8,epoch_);DuplexWire::put(m.bytes,16,8,nonce_);
        DuplexWire::put(m.bytes,24,8,sent_+1);std::copy(payload.begin(),payload.end(),m.bytes.begin()+prefix);
        const auto r=mach_msg(&h,MACH_SEND_MSG|MACH_SEND_TIMEOUT,h.msgh_size,0,MACH_PORT_NULL,0,MACH_PORT_NULL);
        if(r==MACH_SEND_TIMED_OUT)return Result::full; // Not accepted; retain exact payload for retry.
        if(r!=KERN_SUCCESS)return fail(Result::transport);
        ++sent_;return Result::ok;
    }
    Result poll(Packet &out) noexcept {
        if(closed_)return Result::closed;
        if(!valid_)return Result::invalid;
        NativeMachReceived<sizeof(Message)+sizeof(mach_msg_max_trailer_t)> received;
        if(!received.poll(receive_.get(),nativeAuditReceiveOptions)) {
            if(received.status()==MACH_RCV_TIMED_OUT)return Result::idle;
            return fail(Result::transport);
        }
        mach_msg_header_t h;std::memcpy(&h,received.bytes().data(),sizeof(h));
        const auto token=nativeAuditToken(received.bytes());
        if(h.msgh_id!=messageID || h.msgh_size<sizeof(h)+prefix || h.msgh_size>sizeof(Message) ||
           (h.msgh_bits&MACH_MSGH_BITS_COMPLEX) || h.msgh_remote_port!=MACH_PORT_NULL ||
           h.msgh_voucher_port!=MACH_PORT_NULL || h.msgh_local_port!=receive_.get() ||
           !token || std::memcmp(&*token,&expected_,sizeof(expected_))!=0)return fail(Result::invalid);
        const auto bytes=std::span(reinterpret_cast<const uint8_t *>(received.bytes().data())+sizeof(h),h.msgh_size-sizeof(h));
        const auto kind=DuplexWire::get(bytes,0,4),size=DuplexWire::get(bytes,4,4);
        if(kind<1 || kind>8 || size!=bytes.size()-prefix || size%4 ||
           DuplexWire::get(bytes,8,8)!=epoch_ || DuplexWire::get(bytes,16,8)!=nonce_ ||
           received_==UINT64_MAX || DuplexWire::get(bytes,24,8)!=received_+1)return fail(Result::invalid);
        // Do not change caller output on failure. Owned receive destroys all
        // unsolicited reply/voucher/descriptor rights on rejection as well.
        out.kind=Kind(kind);out.used=size;std::copy(bytes.begin()+prefix,bytes.end(),out.bytes.begin());
        ++received_;return Result::ok;
    }
    bool close() noexcept {
        closed_=true;const auto a=receive_.close(),b=peer_.close();
        cleanupOK_=a==KERN_SUCCESS && b==KERN_SUCCESS && cleanupOK_;return cleanupOK_;
    }
};
}
