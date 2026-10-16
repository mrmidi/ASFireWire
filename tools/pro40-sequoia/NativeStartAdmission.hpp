// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeStartNegotiation.hpp"
namespace Pro40Audio {
// Serialized source-side resource ordering, not an authenticated Mach endpoint.
// Producer outlives this object. Do not start its sender until activate succeeds.
// After activation the supervisor owns ongoing producer health and cancellation.
template<class Producer,class Packet=NativeStartPacket> class NativeSourceAdmission {
    using Negotiation=BasicNativeStartNegotiation<Packet>;
    using Descriptor=typename Packet::Descriptor;
public:
    NativeSourceAdmission(Producer &producer,SourceFrameClock::Identity id,uint64_t nonce)
        :producer_(producer),negotiation_(Negotiation::Role::source,id,nonce){}
    ~NativeSourceAdmission(){if(!active_)cancel();}
    NativeSourceAdmission(const NativeSourceAdmission &)=delete;
    NativeSourceAdmission &operator=(const NativeSourceAdmission &)=delete;
    bool offer(uint64_t now,std::span<uint8_t> out)noexcept{
        Descriptor d;
        if(failed_||!producer_.prepare(now,d)||!negotiation_.offer(d,now,out))return fail();return true;
    }
    bool commit(std::span<const uint8_t> echo,uint64_t now,std::span<uint8_t> out)noexcept{
        Descriptor d;
        if(failed_||!negotiation_.confirm(echo,now,d)||!producer_.accept(d,now)||!negotiation_.commit(now,out))return fail();
        return true;
    }
    bool activate(std::span<const uint8_t> ready,uint64_t now)noexcept{
        if(failed_||!poll(now)||!negotiation_.receiveReady(ready,now))return fail();active_=true;return true;
    }
    bool poll(uint64_t now)noexcept{
        if(failed_)return false;
        if(active_)return true; // Admission lifetime ended; not a health check.
        if(!negotiation_.poll(now)||producer_.pump(now)==Producer::Result::failed)return fail();return true;
    }
    bool active()const noexcept{return active_&&!failed_;}
    void cancel()noexcept{fail();}
private:
    bool fail()noexcept{if(!failed_){failed_=true;negotiation_.cancel();producer_.cancel();}return false;}
    Producer &producer_;Negotiation negotiation_;bool failed_=false,active_=false;
};
// Gate is an unpublished receiver resource owner: prepare(d), commit(d), and
// rollback() return bool. It outlives this object. Rollback must quiesce readers
// before freeing resources; false is retained as cleanupOK=false, never retried
// blindly. No READY bytes escape on gate failure. READY send failure requires
// cancel(); dropped replies require supervisor timeout/cancel on BOTH endpoints.
template<class Gate,class Packet=NativeStartPacket> class NativeReceiverAdmission {
    using Negotiation=BasicNativeStartNegotiation<Packet>;
    using Descriptor=typename Packet::Descriptor;
    using Message=typename Packet::Message;
public:
    NativeReceiverAdmission(Gate &gate,SourceFrameClock::Identity id,uint64_t nonce)
        :gate_(gate),negotiation_(Negotiation::Role::receiver,id,nonce){}
    ~NativeReceiverAdmission(){if(!released_)cancel();}
    NativeReceiverAdmission(const NativeReceiverAdmission &)=delete;
    NativeReceiverAdmission &operator=(const NativeReceiverAdmission &)=delete;
    bool echo(std::span<const uint8_t> offer,uint64_t now,std::span<uint8_t> out)noexcept{
        std::array<uint8_t,Packet::size> candidate;Message m;
        if(failed_||out.size()!=candidate.size()||!negotiation_.echo(offer,now,candidate)||
           !Packet::decode(candidate,m))return fail();
        touched_=true;
        if(!gate_.prepare(m.descriptor))return fail();
        std::copy(candidate.begin(),candidate.end(),out.begin());return true;
    }
    bool commit(std::span<const uint8_t> commit,uint64_t now,std::span<uint8_t> out)noexcept{
        Descriptor d;std::array<uint8_t,Packet::size> candidate;
        if(failed_||out.size()!=candidate.size()||!negotiation_.receiveCommit(commit,now,d)||
           !gate_.commit(d)||!negotiation_.ready(now,candidate))return fail();
        std::copy(candidate.begin(),candidate.end(),out.begin());committed_=true;return true;
    }
    bool poll(uint64_t now)noexcept{return !failed_&&negotiation_.poll(now)?true:fail();}
    // Supervisor calls only after READY delivery/activation, transferring ongoing
    // receiver lifetime responsibility. Merely producing READY is not sufficient.
    bool release(uint64_t now)noexcept{if(failed_||!committed_||!poll(now))return fail();released_=true;return true;}
    bool cleanupOK()const noexcept{return cleanupOK_;}
    void cancel()noexcept{fail();}
private:
    bool fail()noexcept{
        if(!failed_){failed_=true;negotiation_.cancel();if(touched_)cleanupOK_=gate_.rollback();}return false;
    }
    Gate &gate_;Negotiation negotiation_;
    bool touched_=false,committed_=false,released_=false,failed_=false,cleanupOK_=true;
};
}
