// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cmath>
#include <cstdint>
#include <cstddef>

namespace Pro40Audio {
enum class TimelineResult {ok, invalidAnchor, wrongEpoch, wrongClock, invalidTime, invalidRange};
// An externally established, immutable same-rate mapping, NOT a clock estimator.
// Feed frame feedFrame corresponds exactly to callback sample callbackSample.
// Rebuild for every feed epoch/clock seed change; never infer from latest data.
struct AudioCallbackTimeline {
    static constexpr int64_t maxExactSample=9007199254740991LL;
    static constexpr size_t maxFrames=4096;
    uint64_t feedEpoch=0,clockSeed=0,feedFrame=0;
    int64_t callbackSample=0;
    TimelineResult map(uint64_t epoch,uint64_t seed,double sample,bool sampleValid,
                       size_t frames,uint64_t &first) const noexcept {
        if(!feedEpoch || !clockSeed || callbackSample < -maxExactSample || callbackSample>maxExactSample)
            return TimelineResult::invalidAnchor;
        if(epoch!=feedEpoch)return TimelineResult::wrongEpoch;
        if(seed!=clockSeed)return TimelineResult::wrongClock;
        if(!sampleValid || !std::isfinite(sample) || sample < -double(maxExactSample) ||
           sample>double(maxExactSample) || std::trunc(sample)!=sample)return TimelineResult::invalidTime;
        if(!frames || frames>maxFrames)return TimelineResult::invalidRange;
        const auto integer=int64_t(sample); // Validated before conversion.
        if(integer>maxExactSample-int64_t(frames))return TimelineResult::invalidRange;
        const int64_t delta=integer-callbackSample; // Both bounded to ±(2^53-1).
        uint64_t mapped=feedFrame;
        if(delta<0) {
            const auto amount=uint64_t(-delta);
            if(amount>mapped)return TimelineResult::invalidRange;
            mapped-=amount;
        } else {
            if(uint64_t(delta)>UINT64_MAX-mapped)return TimelineResult::invalidRange;
            mapped+=uint64_t(delta);
        }
        if(frames>UINT64_MAX-mapped)return TimelineResult::invalidRange;
        first=mapped;return TimelineResult::ok;
    }
};
}
