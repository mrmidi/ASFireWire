// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "VerifierIdentityWork.hpp"
#include "../NativeIdentityCodec.hpp"
namespace Pro40Lab {
// Staged byte boundary for the authenticated serial XPC connection. Authentication
// is an independently established runtime fact, never a field from the message.
// Reuses the private identity codec without changing the lifecycle protocol.
class VerifierIdentityTransport final {
    VerifierIdentityWork work_;
    bool consumed_=false,cancelled_=false;
public:
    VerifierIdentityTransport(std::array<uint8_t,20> policy,LaunchNonce nonce,uint64_t deadline):
        work_(policy,nonce,deadline){}
    void cancel(){consumed_=true;cancelled_=true;}
    template<class Now,class Stopped,class Check>
    [[nodiscard]] std::optional<Pro40Audio::IdentityResultBytes> receive(
        std::span<const uint8_t> bytes,bool authenticated,Now now,Stopped stopped,Check check){
        if(consumed_)return {};
        consumed_=true;
        if(!authenticated)return {};
        auto request=Pro40Audio::decodeIdentityRequest(bytes);
        if(!request)return {};
        auto result=work_.execute(*request,now,[&]{return cancelled_ || stopped();},check);
        if(!result)return {};
        return Pro40Audio::encodeIdentityResult(*result);
    }
};

// This receipt is protocol evidence only. Even status zero cannot admit an audio
// session: independent helper exit/removal/cleanup must still be established.
class VerifierIdentityReply final {
    const Pro40Audio::NativeIdentityRequest expected_;
    const LaunchNonce nonce_;
    const uint64_t deadline_;
    uint64_t sentAt_=0;
    enum class Phase { fresh,pending,consumed } phase_=Phase::fresh;
public:
    VerifierIdentityReply(Pro40Audio::NativeIdentityRequest expected,LaunchNonce nonce,uint64_t deadline):
        expected_(expected),nonce_(nonce),deadline_(deadline){}
    VerifierIdentityReply(const VerifierIdentityReply&)=delete;
    VerifierIdentityReply& operator=(const VerifierIdentityReply&)=delete;
    void cancel(){phase_=Phase::consumed;}
    [[nodiscard]] std::optional<Pro40Audio::IdentityRequestBytes> begin(bool authenticated,uint64_t now){
        const bool fresh=phase_==Phase::fresh;phase_=Phase::consumed;
        if(!fresh || !authenticated || !deadline_ || now>=deadline_ ||
           !expected_.valid() || expected_.requestID!=nonce_)return {};
        sentAt_=now;phase_=Phase::pending;
        return Pro40Audio::encodeIdentityRequest(expected_);
    }
    [[nodiscard]] std::optional<Pro40Audio::NativeIdentityResult> receive(
        std::span<const uint8_t> bytes,bool authenticated,uint64_t now){
        const bool pending=phase_==Phase::pending;phase_=Phase::consumed;
        if(!pending || !authenticated || now<sentAt_ || now>=deadline_)return {};
        auto result=Pro40Audio::decodeIdentityResult(bytes);
        if(!result || !(result->request==expected_))return {};
        return result;
    }
};
}
