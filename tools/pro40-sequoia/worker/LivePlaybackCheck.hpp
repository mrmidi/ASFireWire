// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "../AudioCoreAudioWrite.hpp"
#include <chrono>
#include <pthread.h>
#include <thread>
namespace Pro40 {
// Diagnostic application-side producer, not a DAW or a hardware clock. Creates
// small callback-shaped blocks continuously, paced only by queue space. No
// preloaded song, free-running host sample timer or microphone monitoring.
// Every18 seconds: 2s quiet, 6x (1s quiet440Hz on one channel +1s silence), 4s quiet.
// Peak -30dBFS with5ms fades; 137-frame writes deliberately differ from512 nominal.
class LivePlaybackCheck {
    pthread_t thread_{};
    std::atomic<bool> stop_{false};
    bool started_=false,joined_=false;
    uint64_t callbacks_=0;
    static void *produce(void *context) noexcept {
        auto &s=*static_cast<LivePlaybackCheck *>(context);
        while(!s.stop_.load(std::memory_order_acquire) && s.queue.fault()==Pro40Audio::PlaybackQueueResult::ok) {
            if(s.queue.writable()>=137) {if(!s.block())break;}
            else std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        return nullptr;
    }
    bool block() noexcept {
        std::array<float,137*6> samples{};
        const auto first=queue.written();
        constexpr double tau=6.283185307179586476925286766559;
        for(unsigned f=0;f<137;++f) {
            const auto phase=(first+f)%(18*48000);
            if(phase<2*48000 || phase>=14*48000)continue;
            const auto offset=phase-2*48000;
            const auto channel=offset/(2*48000),sample=offset%(2*48000);
            if(sample>=48000)continue;
            const double fade=sample<240?double(sample)/240:sample>=48000-240?double(48000-1-sample)/240:1.;
            samples[f*6+channel]=float((265271./8388608.)*fade*std::sin(tau*440*sample/48000));
        }
        AudioServerPlugInIOCycleInfo cycle{};
        cycle.mOutputTime.mSampleTime=double(first);cycle.mOutputTime.mFlags=kAudioTimeStampSampleTimeValid;
        cycle.mNominalIOBufferFrameSize=512;
        const auto result=Pro40Audio::writeCoreAudioOutput(Pro40Audio::AudioCallbackTimeline{1,1,0,0},queue,1,1,cycle,137,samples);
        if(!result.ok())return false;
        ++callbacks_;return true;
    }
public:
    Pro40Audio::PlaybackPcmQueue<> queue{1};
    Pro40Packets::PlaybackPcmSource source=queue.port();
    LivePlaybackCheck()=default;
    LivePlaybackCheck(const LivePlaybackCheck &)=delete;
    ~LivePlaybackCheck(){finish();}
    bool prepare() noexcept {
        if(started_ || queue.written())return false;
        while(queue.writable()>=137)if(!block())return false;
        return true;
    }
    bool start() noexcept {
        if(started_ || !queue.written() || queue.fault()!=Pro40Audio::PlaybackQueueResult::ok)return false;
        if(pthread_create(&thread_,nullptr,produce,this)!=0)return false;
        started_=true;return true;
    }
    bool finish() noexcept { // ONLY after native teardown, or before any entry.
        stop_.store(true,std::memory_order_release);
        if(started_ && !joined_) {
            if(pthread_join(thread_,nullptr)!=0)return false;
            joined_=true;
        }
        return queue.fault()==Pro40Audio::PlaybackQueueResult::ok;
    }
    uint64_t callbacks() const noexcept {return callbacks_;} // After join only.
};
}
