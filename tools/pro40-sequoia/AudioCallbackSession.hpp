// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioFeedSession.hpp"
#include "AudioCoreAudioRead.hpp"

namespace Pro40Audio {
struct CallbackSessionResult {
    bool acquired=false;
    CallbackReadResult read{};
    bool ok() const noexcept {return acquired && read.ok();}
};
// Construct completely before exposing to callbacks. One non-RT owner calls
// receive/flush/close; readers call readInput only. Anchor never mutates and the
// feed epoch derives from it, so metadata cannot race or describe another feed.
// To replace an anchor: close, quiesce/join ALL callers, then destroy/recreate.
// Close prevents new leases; an already admitted read may finish successfully.
template<size_t Capacity,size_t Channels=1> class AudioCallbackSession {
    const AudioCallbackTimeline timeline_;
    AudioFeedSession<Capacity,Channels> feed_;
public:
    static constexpr size_t maxPacketBytes=AudioFeedPacketFor<Channels>::maxBytes;
    AudioCallbackSession(AudioCallbackTimeline timeline,uint64_t firstFrame):
        timeline_(timeline),feed_(timeline.feedEpoch,firstFrame) {
        uint64_t ignored=0;
        if(timeline_.map(timeline_.feedEpoch,timeline_.clockSeed,
            double(timeline_.callbackSample),true,1,ignored)!=TimelineResult::ok)
            feed_.close();
    }
    SessionResult receive(std::span<const uint8_t> bytes) {return feed_.receive(bytes);}
    // Owner-only worker lifecycle/status interface, never callback telemetry.
    bool closed() const {return feed_.closed();}
    bool pending() const {return feed_.pending();}
    SessionResult terminal() const {return feed_.terminal();}
    uint64_t acceptedNext() const {return feed_.acceptedNext();}
    uint64_t publishedNext() const {return feed_.publishedNext();}
    SessionResult flush() {return feed_.flush();}
    void close() {feed_.close();}
    CallbackSessionResult readInput(uint64_t expectedEpoch,uint64_t expectedClockSeed,
        const AudioServerPlugInIOCycleInfo &cycle,UInt32 actualFrames,
        std::span<float> output) noexcept {
        auto lease=feed_.tryAcquire(); // One bounded attempt; no callback retries.
        if(!lease) {
            std::fill(output.begin(),output.end(),0.0f);return {};
        }
        return {true,readCoreAudioInput(timeline_,*lease,expectedEpoch,
                                       expectedClockSeed,cycle,actualFrames,output)};
    } // Lease released before returning; no raw window/timeline escapes.
};
}
