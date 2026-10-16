// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>

namespace Pro40 {
// The native library exposes 32-bit completion counters. Retain an explicit
// 24-hour guard while using progress-based liveness, not a finite-recording
// deadline. Four summed port counters also fit in unsigned below this guard.
inline constexpr unsigned sustainedCounterLimit=8000u*60*60*24;
static_assert(sustainedCounterLimit%256==0 && uint64_t(sustainedCounterLimit)*4<UINT32_MAX);
class StreamProgressWatchdog {
    std::array<unsigned,4> previous_{};
    std::array<uint64_t,4> changed_{};
    uint64_t previousTime_=0;
    bool fault_=false;
public:
    static constexpr uint64_t stallMicros=576000;
    explicit StreamProgressWatchdog(uint64_t now):changed_{now,now,now,now},previousTime_(now) {}
    bool observe(uint64_t now,const std::array<unsigned,4> &counts) noexcept {
        if(fault_)return false;
        if(now<previousTime_)return !(fault_=true);
        for(unsigned p=0;p<4;++p) {
            if(counts[p]<previous_[p] || counts[p]>=sustainedCounterLimit-512)
                return !(fault_=true);
            if(counts[p]!=previous_[p]){previous_[p]=counts[p];changed_[p]=now;}
            if(now-changed_[p]>stallMicros)return !(fault_=true);
        }
        previousTime_=now;return true;
    }
};
}
