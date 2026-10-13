// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AudioCallbackTimeline.hpp"
#include "AudioInputWindow.hpp"

namespace Pro40Audio {
struct CallbackReadResult {
    TimelineResult timeline=TimelineResult::ok;
    WindowResult window=WindowResult::ok;
    bool ok() const noexcept {return timeline==TimelineResult::ok && window==WindowResult::ok;}
};
// Caller already owns a coherent timeline and a valid pinned immutable lease.
// No scratch allocation, wait or logging. Failures zero the complete
// destination and return a reason: silence alone must never mean success.
template<class Lease> CallbackReadResult readCallback(const AudioCallbackTimeline &timeline,
    const Lease &lease,uint64_t epoch,uint64_t clockSeed,double sample,bool sampleValid,
    std::span<float> output) noexcept {
    if(output.size()%Lease::channels) {
        std::fill(output.begin(),output.end(),0.0f);return {TimelineResult::invalidRange,WindowResult::ok};
    }
    uint64_t first=0;
    const auto mapped=timeline.map(epoch,clockSeed,sample,sampleValid,output.size()/Lease::channels,first);
    if(mapped!=TimelineResult::ok) {
        std::fill(output.begin(),output.end(),0.0f);return {mapped,WindowResult::ok};
    }
    const auto read=lease.readNormalized(epoch,first,output);
    if(read!=WindowResult::ok) {
        std::fill(output.begin(),output.end(),0.0f);return {TimelineResult::ok,read};
    }
    return {};
}
}
