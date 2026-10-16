// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../PlaybackClip.hpp"
#include <cmath>
#include <memory>
#include <new>

namespace Pro40 {
// Finite audible channel check, generated before native IO. Two seconds of
// silence, then one second of 440 Hz on each of six channels in order, with
// one second of silence between them. Peak -30 dBFS; 5 ms fades. No loopback
// or microphone monitoring. Remaining native session time sends silence.
class SixOutputCheck {
public:
    static constexpr unsigned channels=6,rate=48000,seconds=14;
    static constexpr size_t frames=rate*seconds,bytes=frames*channels*3;
    static constexpr int32_t peak=265271;
private:
    std::unique_ptr<uint8_t[]> pcm_{new(std::nothrow) uint8_t[bytes]()};
public:
    SixOutputCheck() {
        if(!pcm_)return;
        constexpr double tau=6.283185307179586476925286766559;
        constexpr unsigned fade=240;
        for(unsigned channel=0;channel<channels;++channel)for(unsigned n=0;n<rate;++n) {
            const double envelope=n<fade?double(n)/fade:
                n>=rate-fade?double(rate-1-n)/fade:1.0;
            const auto value=int32_t(std::lround(peak*envelope*std::sin(tau*440*n/rate)));
            const size_t frame=(2+2*channel)*rate+n,at=(frame*channels+channel)*3;
            const auto bits=uint32_t(value)&0xffffff;
            for(unsigned byte=0;byte<3;++byte)pcm_[at+byte]=uint8_t(bits>>(byte*8));
        }
    }
    bool valid() const noexcept {return bool(pcm_);}
    Pro40Packets::PlaybackClip clip() const noexcept {
        return {{pcm_.get(),pcm_?bytes:0},channels};
    }
};
}
