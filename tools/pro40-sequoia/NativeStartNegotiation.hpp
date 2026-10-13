// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeStartPacket.hpp"
namespace Pro40Audio {
// Serialized one-shot control model. Both sides receive expected session identity
// and fresh nonzero nonce from the future supervisor/admission owner. Not peer
// authentication. All now values are same-host Mach ticks in the agreed timebase.
// Echoed/confirmed authorize no HAL publication or hardware action on their own.
template<class Packet> class BasicNativeStartNegotiation {
public:
    using Descriptor=typename Packet::Descriptor;
    using Message=typename Packet::Message;
    enum class Role {source,receiver};
    enum class State {empty,offered,echoed,confirmed,committed,active,failed};
    BasicNativeStartNegotiation(Role role,SourceFrameClock::Identity identity,uint64_t nonce)
        :role_(role),identity_(identity),nonce_(nonce){
        if(!nonce||!identity.epoch||!identity.seed||!identity.numer||!identity.denom)fail();
    }
    State state()const noexcept{return state_;}
    BasicNativeStartNegotiation(const BasicNativeStartNegotiation &)=delete;
    BasicNativeStartNegotiation &operator=(const BasicNativeStartNegotiation &)=delete;
    bool offer(const Descriptor &descriptor,uint64_t now,std::span<uint8_t> out)noexcept{
        Message m{Message::Kind::offer,nonce_,descriptor};
        if(role_!=Role::source||state_!=State::empty||!time(now)||!matches(m,now)||!Packet::encode(m,out))return fail();
        proposal_=descriptor;state_=State::offered;return true;
    }
    bool echo(std::span<const uint8_t> offer,uint64_t now,std::span<uint8_t> out)noexcept{
        Message m;
        if(role_!=Role::receiver||state_!=State::empty||!time(now)||!Packet::decode(offer,m)||
           m.kind!=Message::Kind::offer||!matches(m,now))return fail();
        m.kind=Message::Kind::echo;
        if(!Packet::encode(m,out))return fail();
        proposal_=m.descriptor;state_=State::echoed;return true;
    }
    bool confirm(std::span<const uint8_t> echo,uint64_t now,Descriptor &out)noexcept{
        Message m;
        if(role_!=Role::source||state_!=State::offered||!time(now)||!Packet::decode(echo,m)||
           m.kind!=Message::Kind::echo||!matches(m,now)||m.descriptor!=proposal_)return fail();
        out=proposal_;state_=State::confirmed;return true;
    }
    // Caller must accept the source before sending COMMIT, and commit receiver
    // resources before sending READY. NativeStartAdmission enforces that order.
    bool commit(uint64_t now,std::span<uint8_t> out)noexcept{
        if(role_!=Role::source||state_!=State::confirmed||!poll(now)||
           !Packet::encode({Message::Kind::commit,nonce_,proposal_},out))return fail();
        state_=State::committed;return true;
    }
    bool receiveCommit(std::span<const uint8_t> bytes,uint64_t now,Descriptor &out)noexcept{
        if(role_!=Role::receiver||state_!=State::echoed||!receive(bytes,Message::Kind::commit,now))return fail();
        out=proposal_;state_=State::committed;return true;
    }
    bool ready(uint64_t now,std::span<uint8_t> out)noexcept{
        if(role_!=Role::receiver||state_!=State::committed||!poll(now)||
           !Packet::encode({Message::Kind::ready,nonce_,proposal_},out))return fail();
        state_=State::active;return true;
    }
    bool receiveReady(std::span<const uint8_t> bytes,uint64_t now)noexcept{
        if(role_!=Role::source||state_!=State::committed||!receive(bytes,Message::Kind::ready,now))return fail();
        state_=State::active;return true;
    }
    // Retaining an echoed/confirmed value does not extend its admission lifetime.
    bool poll(uint64_t now)noexcept{
        if(!time(now))return fail();
        if(state_!=State::empty&&!proposal_.validAt(now))return fail();return true;
    }
    void cancel()noexcept{fail();}
private:
    bool receive(std::span<const uint8_t> bytes,Message::Kind kind,uint64_t now)noexcept{
        Message m;
        return time(now)&&Packet::decode(bytes,m)&&m.kind==kind&&matches(m,now)&&m.descriptor==proposal_;
    }
    bool matches(const Message &m,uint64_t now)const noexcept{
        return m.nonce==nonce_&&m.descriptor.clock.identity==identity_&&m.descriptor.validAt(now);
    }
    bool time(uint64_t now)noexcept{
        if(state_==State::failed||now<lastTime_)return false;lastTime_=now;return true;
    }
    bool fail()noexcept{state_=State::failed;return false;}
    Role role_;SourceFrameClock::Identity identity_;uint64_t nonce_,lastTime_=0;
    State state_=State::empty;Descriptor proposal_{};
};
using NativeStartNegotiation=BasicNativeStartNegotiation<NativeStartPacket>;
}
