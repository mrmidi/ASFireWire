// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
namespace Pro40Audio {
// Pure immutable timestamp math, NOT a fit, quality gate or native HAL driver.
// All host values use the identified Mach timebase. No bus-wrap decoding here.
struct SourceFrameClock {
    struct Identity {
        uint64_t epoch=0,seed=0;
        uint32_t generation=0,numer=0,denom=0;
        bool operator==(const Identity &) const =default;
    };
    struct Stamp {double sample=0;uint64_t host=0,seed=0;};
    static constexpr uint64_t maxExactFrame=(uint64_t(1)<<53)-1;
    static constexpr uint64_t period=12000; // Existing HAL lab timestamp period.
    Identity identity;
    uint64_t origin=0,ticksNumerator=0,ticksDenominator=0,horizonFrames=0;
private:
    using Wide=__uint128_t;
    Wide offset(uint64_t frame) const noexcept {
        // Product bounded by (2^53-1)*(2^64-1), including rounding addition.
        return (Wide(frame)*ticksNumerator+ticksDenominator-1)/ticksDenominator;
    }
public:
    bool valid() const noexcept {
        if(!identity.epoch || !identity.seed || !identity.numer || !identity.denom || !origin ||
           !ticksDenominator || ticksNumerator<ticksDenominator || !horizonFrames || horizonFrames>maxExactFrame)return false;
        return offset(horizonFrames)<=UINT64_MAX-origin;
    }
    // Round toward the future: a frame is never announced before its rational
    // timestamp. No accumulated per-period rounding. Output unchanged on error.
    bool hostTime(const Identity &expected,uint64_t frame,uint64_t &host) const noexcept {
        if(!valid() || expected!=identity || frame>horizonFrames)return false;
        host=origin+uint64_t(offset(frame));return true;
    }
    // Latest period boundary whose rounded timestamp is <= now. Clock horizon
    // is independent of PCM END, allowing post-END silence with a running clock.
    // Stateless: callers must supply monotonic now; no backward-input masking.
    bool timestamp(const Identity &expected,uint64_t now,Stamp &out) const noexcept {
        if(!valid() || expected!=identity || now<origin || now>origin+uint64_t(offset(horizonFrames)))return false;
        const Wide available=Wide(now-origin)*ticksDenominator/ticksNumerator;
        const uint64_t frame=uint64_t(available/period)*period;
        if(frame>horizonFrames)return false;
        out={double(frame),origin+uint64_t(offset(frame)),identity.seed};return true;
    }
};
}
