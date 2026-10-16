// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <span>

namespace Pro40Packets {
// Immutable interleaved signed 24-bit little-endian PCM. Storage is prepared
// before native entry and outlives the synchronous session. Content only: no
// DMA views, host timers, allocation, locks or I/O in the packet path.
struct PlaybackClip {
    std::span<const uint8_t> pcm;
    unsigned channels=0;
    bool valid() const noexcept {
        return channels>=1 && channels<=12 && !pcm.empty() && pcm.size()%(channels*3)==0;
    }
    size_t frames() const noexcept {return valid()?pcm.size()/(channels*3):0;}
    uint32_t sampleBits(size_t frame,unsigned channel) const noexcept {
        if(channel>=channels || frame>=frames())return 0;
        const auto *p=pcm.data()+(frame*channels+channel)*3;
        return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16;
    }
};
}
