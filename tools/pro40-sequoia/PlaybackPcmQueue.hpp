// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PlaybackPcmSource.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace Pro40Audio {
enum class PlaybackQueueResult : uint32_t {ok, full, identity, range, discontinuity, sample, underrun, state};
// One producer (callback OR IPC owner) and one native consumer. Fixed storage;
// absolute frame IDs do not wrap when the storage does. The producer may retry
// FULL without changing data. All other errors are terminal. A callback that
// cannot retain/retry its input must explicitly fail on FULL too.
// Construct/touch before native entry; stop both owners before destruction.
template<size_t Capacity=8192,size_t Channels=6> class PlaybackPcmQueue {
    static_assert(Capacity>=8 && Channels>=1 && Channels<=12);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(std::atomic<PlaybackQueueResult>::is_always_lock_free);
    std::array<int32_t,Capacity*Channels> samples_{};
    const uint64_t epoch_,origin_;
    alignas(64) std::atomic<uint64_t> written_;
    alignas(64) std::atomic<uint64_t> consumed_;
    std::atomic<PlaybackQueueResult> fault_{PlaybackQueueResult::ok};
    // Writer-only statistics; read only after producer joins.
    uint64_t highWater_=0,clamped_=0;
    // Consumer-only read/commit transaction and margin; inspect after teardown.
    size_t pending_=0;
    uint64_t lowWater_=UINT64_MAX;
    template<class Sample,class Valid,class Convert> PlaybackQueueResult append(
        uint64_t epoch,uint64_t first,std::span<const Sample> samples,Valid valid,Convert convert,
        bool startupSilence=false) noexcept {
        if(fault()!=PlaybackQueueResult::ok)return fault();
        if(epoch!=epoch_)return fail(PlaybackQueueResult::identity);
        if(samples.empty() || samples.size()%Channels || samples.size()/Channels>Capacity ||
           samples.size()/Channels>4096 || first>UINT64_MAX-samples.size()/Channels)
            return fail(PlaybackQueueResult::range);
        const auto write=written_.load(std::memory_order_relaxed);
        if(first<write || (!startupSilence && first!=write))return fail(PlaybackQueueResult::discontinuity);
        const auto silence=first-write;
        const auto frames=samples.size()/Channels;
        if(silence>Capacity-frames)return fail(PlaybackQueueResult::range);
        // Validate the WHOLE callback/message before any sample becomes visible.
        for(auto value:samples)if(!valid(value))return fail(PlaybackQueueResult::sample);
        const auto read=consumed_.load(std::memory_order_acquire);
        if(silence+frames>Capacity-(write-read))return PlaybackQueueResult::full;
        // Publish the initial gap and complete callback together. Never expose
        // startup silence if validation/capacity rejects the requested audio.
        for(uint64_t frame=0;frame<silence;++frame)for(size_t c=0;c<Channels;++c)
            samples_[((write-origin_+frame)%Capacity)*Channels+c]=0;
        for(size_t frame=0;frame<frames;++frame)for(size_t c=0;c<Channels;++c)
            samples_[((first-origin_+frame)%Capacity)*Channels+c]=convert(samples[frame*Channels+c]);
        if(first-read+frames>highWater_)highWater_=first-read+frames;
        written_.store(first+frames,std::memory_order_release);return PlaybackQueueResult::ok;
    }
public:
    static constexpr size_t channels=Channels,capacity=Capacity;
    PlaybackPcmQueue(uint64_t epoch,uint64_t firstFrame=0)
        :epoch_(epoch),origin_(firstFrame),written_(firstFrame),consumed_(firstFrame) {
        if(!epoch)fail(PlaybackQueueResult::identity);
    }
    PlaybackPcmQueue(const PlaybackPcmQueue &)=delete;
    PlaybackQueueResult fault() const noexcept {return fault_.load(std::memory_order_acquire);}
    PlaybackQueueResult fail(PlaybackQueueResult why) noexcept {
        auto expected=PlaybackQueueResult::ok;
        fault_.compare_exchange_strong(expected,why,std::memory_order_acq_rel);
        return fault();
    }
    uint64_t consumed() const noexcept {return consumed_.load(std::memory_order_acquire);}
    uint64_t written() const noexcept {return written_.load(std::memory_order_acquire);}
    size_t writable() const noexcept { // Producer only, not a coherent third-party snapshot.
        return Capacity-(written_.load(std::memory_order_relaxed)-consumed_.load(std::memory_order_acquire));
    }
    uint64_t highWater() const noexcept {return highWater_;}
    uint64_t lowWater() const noexcept {return lowWater_;}
    uint64_t clamped() const noexcept {return clamped_;}
    PlaybackQueueResult write(uint64_t epoch,uint64_t first,std::span<const int32_t> samples) noexcept {
        return append(epoch,first,samples,[](int32_t v){return v>=-8388608 && v<=8388607;},[](int32_t v){return v;});
    }
    // Only the serialized callback owner may allow a gap on its FIRST write.
    // This does not change any frame ID or replace previously queued audio.
    PlaybackQueueResult writeNormalized(uint64_t epoch,uint64_t first,std::span<const float> samples,
                                        bool startupSilence=false) noexcept {
        return append(epoch,first,samples,[](float v){return std::isfinite(v);},[this](float v){
            // Normalized CoreAudio samples map to signed24; saturate finite
            // over-range values, reject NaN/Inf before converting any sample.
            if(v>1.0f || v<-1.0f)++clamped_;
            if(v>=1.0f)return int32_t(8388607);
            if(v<=-1.0f)return int32_t(-8388608);
            return std::min<int32_t>(8388607,int32_t(std::round(double(v)*8388608.0)));
        },startupSilence);
    }
    bool read(uint64_t first,std::span<int32_t> output) noexcept {
        if(fault()!=PlaybackQueueResult::ok)return false;
        const auto read=consumed_.load(std::memory_order_relaxed);
        if(output.empty() || output.size()%Channels || output.size()/Channels>Capacity ||
           first!=read || first>UINT64_MAX-output.size()/Channels) {
            fail(PlaybackQueueResult::range);return false;
        }
        const auto frames=output.size()/Channels;
        const auto available=written_.load(std::memory_order_acquire)-read;
        if(frames>available){fail(PlaybackQueueResult::underrun);return false;}
        if(pending_ && pending_!=frames){fail(PlaybackQueueResult::state);return false;}
        for(size_t f=0;f<frames;++f)for(size_t c=0;c<Channels;++c)
            output[f*Channels+c]=samples_[((read-origin_+f)%Capacity)*Channels+c];
        pending_=frames;
        if(available<lowWater_)lowWater_=available;
        return true;
    }
    bool commit(uint64_t first,size_t frames) noexcept {
        if(fault()!=PlaybackQueueResult::ok)return false;
        if(!frames || frames!=pending_ || first!=consumed_.load(std::memory_order_relaxed)) {
            fail(PlaybackQueueResult::state);return false;
        }
        pending_=0;consumed_.store(first+frames,std::memory_order_release);return true;
    }
    Pro40Packets::PlaybackPcmSource port() noexcept {
        return {this,Channels,
            [](void *p,uint64_t f,std::span<int32_t> s)noexcept{return static_cast<PlaybackPcmQueue *>(p)->read(f,s);},
            [](void *p,uint64_t f,unsigned n)noexcept{return static_cast<PlaybackPcmQueue *>(p)->commit(f,n);}};
    }
};
}
