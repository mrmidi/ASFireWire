// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace Pro40Audio {
// Single-owner arrival gate. Sequence identifies one immutable evidence prefix;
// accepting arrival does not approve its clock estimate or publish timestamps.
class ClockEvidenceOrder {
public:
    enum class Result {accepted,invalid,future,stale,outOfOrder,expired,unavailable};
    ClockEvidenceOrder(uint64_t maxAgeTicks,uint64_t maxUseAgeTicks)
        :maxAge_(maxAgeTicks),maxUseAge_(maxUseAgeTicks){}
    // Must be checked at every later use, not just at proposal receipt.
    // Once expired, this session cannot be revived by a new observation.
    Result usable(uint64_t now) noexcept {
        if(expired_)return Result::expired;
        if(!have_)return Result::unavailable;
        if(now<lastUse_ || now<arrival_)return Result::outOfOrder;
        lastUse_=now;
        if(now-observed_>maxUseAge_){expired_=true;return Result::expired;}
        return Result::accepted;
    }
    Result accept(uint64_t sequence,uint64_t observedHost,uint64_t arrivalHost) noexcept {
        if(expired_)return Result::expired;
        if(!maxAge_ || maxUseAge_<maxAge_ || !sequence || !observedHost || !arrivalHost)return Result::invalid;
        if(observedHost>arrivalHost)return Result::future;
        if(arrivalHost-observedHost>maxAge_)return Result::stale;
        if(have_ && (sequence<=sequence_ || observedHost<=observed_ || arrivalHost<arrival_ || arrivalHost<lastUse_))return Result::outOfOrder;
        if(have_ && arrivalHost-observed_>maxUseAge_){expired_=true;return Result::expired;}
        sequence_=sequence;observed_=observedHost;arrival_=arrivalHost;lastUse_=arrivalHost;have_=true;
        return Result::accepted;
    }
private:
    uint64_t maxAge_,maxUseAge_,sequence_=0,observed_=0,arrival_=0,lastUse_=0;
    bool have_=false,expired_=false;
};
}
