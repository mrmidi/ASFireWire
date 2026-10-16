// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "RunningAffineFit.hpp"
#include <array>
#include <cstddef>

namespace Pro40Audio {
// Off-realtime, single-owner bounded history. Add is O(1); fit re-centres the
// current window in O(Capacity), avoiding cancellation from subtracting old
// covariance at large absolute frame/host positions. No allocation or IO.
// count in Fit is WINDOW observations, never cumulative session progress.
template<size_t Capacity> class RollingAffineFit {
    static_assert(Capacity>=3);
    struct Point {long double x=0,y=0;};
    std::array<Point,Capacity> points_{};
    size_t next_=0,used_=0;
    uint64_t total_=0,limit_;
    bool fault_=false;
public:
    explicit RollingAffineFit(uint64_t limit):limit_(limit){}
    bool add(long double x,long double y) noexcept {
        if(fault_)return false;
        if(total_>=limit_ || !std::isfinite(x) || !std::isfinite(y))return !(fault_=true);
        points_[next_]={x,y};next_=(next_+1)%Capacity;
        if(used_<Capacity)++used_;
        ++total_;return true;
    }
    bool fit(RunningAffineFit::Fit &out) const noexcept {
        if(fault_ || used_<3)return false;
        RunningAffineFit fit(Capacity);
        const size_t first=used_==Capacity?next_:0;
        for(size_t n=0;n<used_;++n) {
            const auto &p=points_[(first+n)%Capacity];
            if(!fit.add(p.x,p.y))return false;
        }
        return fit.fit(out);
    }
};
}
