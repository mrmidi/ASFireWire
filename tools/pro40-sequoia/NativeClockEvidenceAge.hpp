// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <limits>
namespace Pro40 {
// Diagnostic only; never admits a stream or changes freshness policy. Host read
// bracket gives lower/upper age bounds. Caller supplies the native host timebase
// and marks evidence healthy only after validating its capture metadata.
struct NativeClockEvidenceAge {
    enum class State {absent,invalid,valid};
    State state=State::absent;
    uint64_t lowerMicros=0,upperMicros=0;
    static NativeClockEvidenceAge measure(bool present,bool healthy,uint64_t before,uint64_t after,
            uint64_t now,uint32_t numer,uint32_t denom,uint32_t hostNumer,uint32_t hostDenom) noexcept {
        if(!present)return {};
        NativeClockEvidenceAge out{State::invalid};
        if(!healthy||!numer||!denom||numer!=hostNumer||denom!=hostDenom||before>after||now<after)return out;
        const __uint128_t divisor=__uint128_t(denom)*1000;
        const auto low=__uint128_t(now-after)*numer/divisor;
        const auto high=(__uint128_t(now-before)*numer+divisor-1)/divisor;
        if(high>std::numeric_limits<uint64_t>::max())return out;
        return {State::valid,uint64_t(low),uint64_t(high)};
    }
};
}
