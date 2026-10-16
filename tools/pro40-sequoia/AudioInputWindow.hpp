// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioBlockQueue.hpp"
#include <algorithm>
#include <span>

namespace Pro40Audio {
enum class WindowResult {ok, wrongEpoch, discontinuity, invalidRange, invalidSample, stale, notYetAvailable};

// Single-owner frame-addressed model, NOT a concurrently readable HAL buffer.
// External publication/lifetime control is still required before live use.
// A new session constructs a new window with an explicit epoch and first frame.
template<size_t Capacity,size_t Channels=1> class AudioInputWindow {
    static_assert(Capacity>=8 && Channels>=1 && Channels<=20 && Capacity<=SIZE_MAX/Channels);
    std::array<int32_t,Capacity*Channels> samples_{};
    const uint64_t epoch_;
    uint64_t oldest_,next_;
public:
    static constexpr size_t channels=Channels;
    AudioInputWindow(uint64_t epoch,uint64_t firstFrame):epoch_(epoch),oldest_(firstFrame),next_(firstFrame) {}
    uint64_t epoch() const noexcept {return epoch_;}
    uint64_t oldest() const noexcept {return oldest_;}
    uint64_t next() const noexcept {return next_;}
    WindowResult append(const InputBlock &block) noexcept requires(Channels==1) {
        return append(block.epoch,block.firstFrame,block.inputOne);
    }
    WindowResult append(uint64_t epoch,uint64_t firstFrame,std::span<const int32_t> samples) noexcept {
        if(epoch!=epoch_)return WindowResult::wrongEpoch;
        if(firstFrame!=next_)return WindowResult::discontinuity;
        if(samples.size()!=8*Channels)return WindowResult::invalidRange;
        if(next_>std::numeric_limits<uint64_t>::max()-8)return WindowResult::invalidRange;
        for(const auto sample:samples)
            if(sample < -8388608 || sample > 8388607)return WindowResult::invalidSample;
        // All preflight checks precede mutation. Evict only by absolute age;
        // reading never advances cursors, so overlapping clients see same data.
        for(size_t n=0;n<8;++n)for(size_t c=0;c<Channels;++c)
            samples_[((next_+n)%Capacity)*Channels+c]=samples[n*Channels+c];
        next_+=8;
        if(next_-oldest_>Capacity)oldest_=next_-Capacity;
        return WindowResult::ok;
    }
private:
    template<class Sample,class Convert>
    WindowResult readValues(uint64_t epoch,uint64_t firstFrame,std::span<Sample> output,Convert convert) const noexcept {
        const auto frames=output.size()/Channels;
        WindowResult result=WindowResult::ok;
        if(epoch!=epoch_)result=WindowResult::wrongEpoch;
        else if(output.size()%Channels || frames>Capacity || frames>std::numeric_limits<uint64_t>::max()-firstFrame)
            result=WindowResult::invalidRange;
        else if(firstFrame<oldest_)result=WindowResult::stale;
        else if(firstFrame+frames>next_)result=WindowResult::notYetAvailable;
        if(result!=WindowResult::ok) {
            // Never return a mixture of old audio, partial audio and silence.
            // Caller must also propagate the explicit failure/discontinuity.
            std::fill(output.begin(),output.end(),0);
            return result;
        }
        for(size_t n=0;n<frames;++n)for(size_t c=0;c<Channels;++c)
            output[n*Channels+c]=convert(samples_[((firstFrame+n)%Capacity)*Channels+c]);
        return WindowResult::ok;
    }
public:
    WindowResult read(uint64_t epoch,uint64_t firstFrame,std::span<int32_t> output) const noexcept {
        return readValues(epoch,firstFrame,output,[](int32_t v){return v;});
    }
    WindowResult readNormalized(uint64_t epoch,uint64_t firstFrame,std::span<float> output) const noexcept {
        return readValues(epoch,firstFrame,output,[](int32_t v){return float(v)*(1.0f/8388608.0f);});
    }
};
}
