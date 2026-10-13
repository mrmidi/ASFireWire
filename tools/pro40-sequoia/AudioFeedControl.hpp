// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>
#include <span>
namespace Pro40Audio {
enum class FeedLifecycle { active, drained, timedOut, fault };
// Private process protocol, not FireWire. END producer->owner; ACK callback
// completion->owner. A producer-side ACK must never be treated as completion.
struct AudioFeedControl {
    enum class Kind:uint8_t { end=1,ack=2 };
    Kind kind=Kind::end;
    uint64_t epoch=0,frame=0;
    static constexpr size_t size=32;
    static bool decode(std::span<const uint8_t> raw,AudioFeedControl &out) noexcept {
        if(raw.size()!=size)return false;
        if(raw[0]!='P'||raw[1]!='4'||raw[2]!='E'||raw[3]!='0'||raw[4]!=1||raw[5]<1||raw[5]>2)return false;
        for(size_t n:{6u,7u,24u,25u,26u,27u,28u,29u,30u,31u})if(raw[n])return false;
        auto get=[&](size_t at){uint64_t v=0;for(unsigned n=0;n<8;++n)v|=uint64_t(raw[at+n])<<(8*n);return v;};
        AudioFeedControl value{Kind(raw[5]),get(8),get(16)};
        if(!value.epoch || !value.frame || value.frame>UINT64_MAX-511 ||
           value.frame%(value.kind==Kind::end?8:512))return false;
        out=value;return true;
    }
    bool encode(std::span<uint8_t> raw) const noexcept {
        if(raw.size()!=size)return false;
        std::array<uint8_t,size> b{};b[0]='P';b[1]='4';b[2]='E';b[3]='0';b[4]=1;b[5]=uint8_t(kind);
        for(unsigned n=0;n<8;++n){b[8+n]=uint8_t(epoch>>(8*n));b[16+n]=uint8_t(frame>>(8*n));}
        AudioFeedControl ignored;if(!decode(b,ignored))return false;
        for(size_t n=0;n<size;++n)raw[n]=b[n];return true;
    }
};
}
