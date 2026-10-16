// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace NativePackets {
// Control-owner drain of already published, finite acyclic programs. No new
// descriptors, links or payloads. Completion is an exact callback watermark,
// not elapsed time or a quiet-period guess. Native Stop/finalize/close follows.
enum class StopDrainResult {complete,invalid,timeout};
template<std::size_t N,class Now,class Observe,class Dispatch>
StopDrainResult drainPublishedStops(const std::array<uint64_t,N> &targets,
                                    const auto &deadline,Now now,Observe observe,Dispatch dispatch) {
    std::array<uint64_t,N> previous{};
    for(const auto target:targets)if(!target)return StopDrainResult::invalid;
    for(;;) {
        std::array<uint64_t,N> progress{};
        if(!observe(progress))return StopDrainResult::invalid;
        bool complete=true;
        for(std::size_t n=0;n<N;++n) {
            if(progress[n]<previous[n] || progress[n]>targets[n])return StopDrainResult::invalid;
            if(progress[n]!=targets[n])complete=false;
        }
        if(complete)return StopDrainResult::complete;
        if(now()>=deadline)return StopDrainResult::timeout;
        previous=progress;dispatch();
    }
}
}
