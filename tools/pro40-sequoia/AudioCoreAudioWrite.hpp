// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioCallbackTimeline.hpp"
#include "PlaybackPcmQueue.hpp"
#include <CoreAudio/AudioServerPlugIn.h>
namespace Pro40Audio {
struct CallbackWriteResult {
    TimelineResult timeline=TimelineResult::ok;
    PlaybackQueueResult queue=PlaybackQueueResult::ok;
    bool ok() const noexcept {return timeline==TimelineResult::ok && queue==PlaybackQueueResult::ok;}
};
// Installed AudioServerPlugIn.h:221-223 mOutputTime, 1087-1126 actual frames.
// Call once per mixed output interval on one serialized owner, not per client.
// No allocation, locks, waits or IPC. Mapping was admitted before callbacks;
// this helper never infers a new clock origin from the current queue position.
template<class Queue> CallbackWriteResult writeCoreAudioOutput(const AudioCallbackTimeline &timeline,
    Queue &queue,uint64_t epoch,uint64_t seed,const AudioServerPlugInIOCycleInfo &cycle,
    UInt32 actualFrames,std::span<const float> samples,bool startupSilence=false) noexcept {
    if(samples.size()%Queue::channels || samples.size()/Queue::channels!=actualFrames) {
        queue.fail(PlaybackQueueResult::range);return {TimelineResult::invalidRange,PlaybackQueueResult::range};
    }
    uint64_t first=0;
    const auto mapped=timeline.map(epoch,seed,cycle.mOutputTime.mSampleTime,
        (cycle.mOutputTime.mFlags&kAudioTimeStampSampleTimeValid)!=0,actualFrames,first);
    if(mapped!=TimelineResult::ok) {
        queue.fail(PlaybackQueueResult::identity);return {mapped,PlaybackQueueResult::identity};
    }
    const auto result=queue.writeNormalized(epoch,first,samples,startupSilence);
    // This callback cannot retain an unaccepted buffer and retry it later.
    if(result!=PlaybackQueueResult::ok)queue.fail(result);
    return {TimelineResult::ok,result};
}
}
