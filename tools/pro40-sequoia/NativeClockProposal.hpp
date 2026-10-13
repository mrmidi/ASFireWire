// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeClockFit.hpp"
#include "SourceFrameClock.hpp"
#include <algorithm>
namespace Pro40Audio {
// Pure finite diagnostic conversion. Input must come from NativeClockFit;
// this structural conversion does not approve fit quality or physical accuracy.
inline bool nativeClockProposal(const NativeClockFit::Snapshot &s,
                                SourceFrameClock::Identity identity,uint64_t horizon,
                                SourceFrameClock &out) noexcept {
    if(identity.generation!=s.generation||identity.numer!=s.numer||identity.denom!=s.denom||
       !s.numer||!s.denom||!s.observations||s.packet.count<3||s.bus.count<3||
       !std::isfinite(s.packet.slope)||!std::isfinite(s.packet.intercept)||
       !std::isfinite(s.bus.slope)||!std::isfinite(s.bus.intercept)||
       s.packet.slope<=0||s.bus.slope<=0)return false;
    const long double hostPerSecond=1000000000.L*s.denom/s.numer;
    const long double ticksPerFrame=s.packet.slope/24576000.L/s.bus.slope*hostPerSecond;
    const long double relativeOrigin=((s.packet.intercept+s.packetEpochShiftTicks-
        static_cast<long double>(s.controller.firstBusTicks))/24576000.L-s.bus.intercept)/s.bus.slope*hostPerSecond;
    // Fixed binary denominator gives bounded conversion work. Upward rounding
    // contributes less than one numerator unit per frame (2^-32 host ticks).
    constexpr uint64_t denominator=uint64_t(1)<<32;
    const auto numerator=std::ceil(ticksPerFrame*denominator);
    const auto delta=std::ceil(relativeOrigin);
    const auto limit=std::ldexp(1.L,64);
    if(!std::isfinite(numerator)||numerator<denominator||numerator>=limit||
       !std::isfinite(delta)||delta<=-limit||delta>=limit)return false;
    const __int128 origin=__int128(s.controller.originHost)+static_cast<__int128>(delta);
    if(origin<=0||origin>UINT64_MAX)return false;
    SourceFrameClock candidate{identity,uint64_t(origin),uint64_t(numerator),denominator,horizon};
    if(!candidate.valid())return false;
    out=candidate;return true;
}
// Completed RX is not the same as elapsed presentation time: SYT can name
// future samples (Linux amdtp-stream.c:469-487). Keep every received PCM frame,
// but advertise only complete eight-frame groups whose final rounded host
// timestamp is no later than this controller observation. Fit count/packet
// prefix remain untouched; quality and monotonic-publication gates still apply.
inline bool nativePastClockProposal(const NativeClockSnapshot &s,SourceFrameClock::Identity identity,
    uint64_t receivedFrames,uint64_t lookAheadFrames,SourceFrameClock &out,uint64_t &pastFrames) noexcept {
    if(!receivedFrames || receivedFrames%8 || !lookAheadFrames ||
       lookAheadFrames>SourceFrameClock::maxExactFrame ||
       receivedFrames>SourceFrameClock::maxExactFrame-lookAheadFrames)return false;
    SourceFrameClock proposal;
    if(!nativeClockProposal(s,identity,receivedFrames+lookAheadFrames,proposal))return false;
    uint64_t count=0;
    if(s.controller.lastHost>=proposal.origin) {
        const auto latest=__uint128_t(s.controller.lastHost-proposal.origin)*proposal.ticksDenominator/proposal.ticksNumerator;
        count=uint64_t(std::min<__uint128_t>(receivedFrames,latest+1))/8*8;
    }
    proposal.horizonFrames=count+lookAheadFrames;
    if(!proposal.valid())return false;
    out=proposal;pastFrames=count;return true;
}
}
