// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "VerifierLaunchPolicy.hpp"
#include <optional>
#include <span>
namespace Pro40Lab {
enum class ExchangeKind : uint8_t { challenge=1, ready=2, release=3, released=4 };
struct ExchangeMessage { ExchangeKind kind; LaunchNonce nonce; };
using ExchangeFrame = std::array<uint8_t,24>;
inline ExchangeFrame encodeExchange(ExchangeMessage m) {
    ExchangeFrame f{0x50,0x34,0x56,0x58,1,static_cast<uint8_t>(m.kind),0,0};
    for(size_t i=0;i<m.nonce.size();++i)f[8+i]=m.nonce[i];
    return f;
}
inline std::optional<ExchangeMessage> decodeExchange(std::span<const uint8_t> f) {
    if(f.size()!=24 || f[0]!=0x50 || f[1]!=0x34 || f[2]!=0x56 || f[3]!=0x58 ||
       f[4]!=1 || f[5]<1 || f[5]>4 || f[6] || f[7])return {};
    ExchangeMessage m{static_cast<ExchangeKind>(f[5]),{}};bool nonzero=false;
    for(size_t i=0;i<m.nonce.size();++i){m.nonce[i]=f[8+i];nonzero|=m.nonce[i]!=0;}
    return nonzero?std::optional(m):std::nullopt;
}
// Serial, single authenticated connection only. Authentication is established
// by runtime peer requirements before activation, never by a frame field.
// Readiness/released acknowledgement proves protocol progress, NOT process exit.
class VerifierExchange {
    const bool helper_;
    const uint64_t deadline_;
    LaunchNonce nonce_;
    uint64_t last_=0;
    unsigned phase_=0; // helper: challenge, release, done; controller: ready, release-allowed, released, done
    bool blocked_=false;
    bool check(uint64_t now) {
        if(blocked_ || !deadline_ || now>=deadline_ || now<last_){blocked_=true;return false;}
        last_=now;return true;
    }
public:
    VerifierExchange(bool helper, LaunchNonce nonce,uint64_t deadline):helper_(helper),deadline_(deadline),nonce_(nonce) {}
    VerifierExchange(const VerifierExchange&)=delete;
    VerifierExchange& operator=(const VerifierExchange&)=delete;
    void cancel(){blocked_=true;}
    bool done()const{return !blocked_ && phase_==(helper_?2U:3U);}
    std::optional<ExchangeFrame> receive(std::span<const uint8_t> frame,bool authenticated,uint64_t now) {
        if(!check(now))return {};
        auto m=decodeExchange(frame);
        if(!authenticated || !m){blocked_=true;return {};}
        if(helper_) {
            if(phase_==0 && m->kind==ExchangeKind::challenge) {
                nonce_=m->nonce;phase_=1;return encodeExchange({ExchangeKind::ready,nonce_});
            }
            if(phase_==1 && m->kind==ExchangeKind::release && m->nonce==nonce_) {
                phase_=2;return encodeExchange({ExchangeKind::released,nonce_});
            }
        } else if(m->nonce==nonce_) {
            if(phase_==0 && m->kind==ExchangeKind::ready){phase_=1;return encodeExchange(*m);}
            if(phase_==2 && m->kind==ExchangeKind::released){phase_=3;return encodeExchange(*m);}
        }
        blocked_=true;return {};
    }
    // Owner calls only inside the durable release gate, after independent PID/
    // birth/run observations. No automatic release upon receiving readiness.
    std::optional<ExchangeFrame> release(bool durableAndObserved,uint64_t now) {
        if(!check(now))return {};
        if(helper_ || phase_!=1 || !durableAndObserved){blocked_=true;return {};}
        phase_=2;return encodeExchange({ExchangeKind::release,nonce_});
    }
};
}
