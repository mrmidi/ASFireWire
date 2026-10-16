// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cmath>
#include <cstdint>
namespace Pro40Audio {
// Fixed-storage cumulative least squares. Inputs must already have validated
// identity, continuity, units and wrap interpretation. No quality admission.
class RunningAffineFit {
public:
    struct Fit {long double slope=0,intercept=0;uint64_t count=0;};
    explicit RunningAffineFit(uint64_t limit):limit_(limit){}
    bool add(long double x,long double y) noexcept {
        if(fault_)return false;
        if(count_>=limit_ || !std::isfinite(x)||!std::isfinite(y)){fault_=true;return false;}
        if(!count_){x0_=x;y0_=y;}
        x-=x0_;y-=y0_;
        const auto n=count_+1;
        const auto dx=x-mx_,dy=y-my_;
        const auto mx=mx_+dx/n,my=my_+dy/n;
        const auto xx=xx_+dx*(x-mx),xy=xy_+dx*(y-my);
        if(!std::isfinite(mx)||!std::isfinite(my)||!std::isfinite(xx)||!std::isfinite(xy)){
            fault_=true;return false;
        }
        mx_=mx;my_=my;xx_=xx;xy_=xy;count_=n;return true;
    }
    bool fit(Fit &out) const noexcept {
        if(fault_ || count_<3 || !(xx_>0))return false;
        const auto slope=xy_/xx_,intercept=y0_+my_-slope*(x0_+mx_);
        if(!std::isfinite(slope)||!std::isfinite(intercept))return false;
        out={slope,intercept,count_};return true;
    }
private:
    uint64_t limit_,count_=0;long double x0_=0,y0_=0,mx_=0,my_=0,xx_=0,xy_=0;
    bool fault_=false;
};
}
