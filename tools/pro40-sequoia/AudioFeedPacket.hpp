// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Pro40Audio {
// Our private IPC payload, NOT FireWire framing or a mapped C++ struct.
// Copy complete messages off the realtime path. No pointers/atomics cross IPC.
template<size_t Channels> struct AudioFeedBatchFor {
    static_assert(Channels>=1 && Channels<=20);
    static constexpr size_t channels=Channels;
    static constexpr uint32_t maxFrames=512;
    uint64_t epoch=0,firstFrame=0;
    uint32_t frames=0;
    // Frame-major interleaved signed PCM24. Ordinals/counts always mean frames.
    std::array<int32_t,maxFrames*Channels> samples{};
};
enum class AudioFeedError {ok, size, format, epoch, range, sample};
template<size_t Channels> class AudioFeedPacketFor {
    using Batch=AudioFeedBatchFor<Channels>;
    static uint64_t get(std::span<const uint8_t> bytes,size_t at,size_t count) {
        uint64_t value=0;for(size_t n=0;n<count;++n)value|=uint64_t(bytes[at+n])<<(8*n);return value;
    }
    static void put(std::span<uint8_t> bytes,size_t at,size_t count,uint64_t value) {
        for(size_t n=0;n<count;++n)bytes[at+n]=uint8_t(value>>(8*n));
    }
    static AudioFeedError validate(const Batch &batch) {
        if(!batch.epoch)return AudioFeedError::epoch;
        if(!batch.frames || batch.frames>Batch::maxFrames || batch.frames%8)return AudioFeedError::range;
        if(batch.firstFrame>UINT64_MAX-batch.frames)return AudioFeedError::range;
        for(size_t n=0;n<batch.frames*Channels;++n)
            if(batch.samples[n]<-8388608 || batch.samples[n]>8388607)return AudioFeedError::sample;
        return AudioFeedError::ok;
    }
public:
    static constexpr size_t headerBytes=40,maxBytes=headerBytes+4*Batch::maxFrames*Channels;
    // Failure leaves output bytes and used unchanged, including short output.
    static AudioFeedError encode(const Batch &batch,std::span<uint8_t> output,size_t &used) {
        const auto valid=validate(batch);if(valid!=AudioFeedError::ok)return valid;
        const size_t size=headerBytes+4*batch.frames*Channels;if(output.size()<size)return AudioFeedError::size;
        put(output,0,4,0x30463450); // ASCII P4F0
        put(output,4,2,1);put(output,6,2,headerBytes);
        put(output,8,4,size);put(output,12,4,batch.frames);
        put(output,16,8,batch.epoch);put(output,24,8,batch.firstFrame);
        put(output,32,4,48000);put(output,36,2,Channels);put(output,38,2,1);
        for(size_t n=0;n<batch.frames*Channels;++n)put(output,headerBytes+4*n,4,std::bit_cast<uint32_t>(batch.samples[n]));
        used=size;return AudioFeedError::ok;
    }
    // Decode into owned storage only after every field/sample validates. Exact
    // size forbids trailing bytes, truncation, ambiguous padding or partial data.
    static AudioFeedError decode(std::span<const uint8_t> bytes,Batch &output) {
        if(bytes.size()<headerBytes || bytes.size()>maxBytes)return AudioFeedError::size;
        if(get(bytes,0,4)!=0x30463450 || get(bytes,4,2)!=1 || get(bytes,6,2)!=headerBytes ||
           get(bytes,32,4)!=48000 || get(bytes,36,2)!=Channels || get(bytes,38,2)!=1)return AudioFeedError::format;
        if(get(bytes,8,4)!=bytes.size())return AudioFeedError::size;
        Batch batch;batch.frames=uint32_t(get(bytes,12,4));
        if(!batch.frames || batch.frames>Batch::maxFrames || batch.frames%8)return AudioFeedError::range;
        if(bytes.size()!=headerBytes+4*batch.frames*Channels)return AudioFeedError::size;
        batch.epoch=get(bytes,16,8);batch.firstFrame=get(bytes,24,8);
        for(size_t n=0;n<batch.frames*Channels;++n)
            batch.samples[n]=std::bit_cast<int32_t>(uint32_t(get(bytes,headerBytes+4*n,4)));
        const auto valid=validate(batch);if(valid!=AudioFeedError::ok)return valid;
        output=batch;return AudioFeedError::ok;
    }
};
// Existing mono labs retain their exact wire representation through aliases;
// all geometries share one codec. Expected channels are fixed by admission,
// never learned from an untrusted first packet or changed during a session.
using AudioFeedBatch=AudioFeedBatchFor<1>;
using AudioFeedPacket=AudioFeedPacketFor<1>;
}
