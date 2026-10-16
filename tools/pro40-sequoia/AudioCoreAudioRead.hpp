// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioCallbackRead.hpp"
#include <CoreAudio/AudioServerPlugIn.h>

namespace Pro40Audio {
// Installed SDK AudioServerPlugIn.h:218–220 mInputTime starts input data;
// DoIOOperation inIOBufferFrameSize is actual, potentially unlike nominal size.
// Caller supplies a valid output span and owns the pinned lease for this call.
template<class Lease> CallbackReadResult readCoreAudioInput(const AudioCallbackTimeline &timeline,
    const Lease &lease,uint64_t epoch,uint64_t clockSeed,const AudioServerPlugInIOCycleInfo &cycle,
    UInt32 actualFrames,std::span<float> output) noexcept {
    if(output.size()%Lease::channels || output.size()/Lease::channels!=actualFrames) {
        std::fill(output.begin(),output.end(),0.0f);
        return {TimelineResult::invalidRange,WindowResult::ok};
    }
    return readCallback(timeline,lease,epoch,clockSeed,cycle.mInputTime.mSampleTime,
        (cycle.mInputTime.mFlags&kAudioTimeStampSampleTimeValid)!=0,output);
}
}
