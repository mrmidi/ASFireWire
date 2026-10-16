// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <span>
namespace Pro40Packets {
// Content scheduling evidence, not DMA completion or measured DAC latency.
// cycle is normalized seconds*8000+cycle modulo8s; syt is the encoded CIP SYT.
struct PlaybackPacketPosition {
    uint64_t frame=0,packetOrdinal=0;
    uint32_t cycle=0;
    uint16_t syt=0;
    uint8_t frames=0;
};
// Content-only port. The synchronous native owner reads eight frame-major
// signed PCM24 frames, encodes the packet, then commits exactly that read.
// Owner/context outlive native teardown. No allocation, IO, locks or waiting.
// Capacity retries and no-data packets must not call either function.
struct PlaybackPcmSource {
    void *context=nullptr;
    unsigned channels=0;
    bool (*read)(void *,uint64_t,std::span<int32_t>) noexcept=nullptr;
    bool (*commit)(void *,uint64_t,unsigned) noexcept=nullptr;
    void (*prepared)(void *,const PlaybackPacketPosition &) noexcept=nullptr;
    bool valid() const noexcept {return context && channels>=1 && channels<=12 && read && commit;}
};
}
