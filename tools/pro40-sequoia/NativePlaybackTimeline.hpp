// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "NativeClockFit.hpp"
#include "SourceFrameClock.hpp"
#include "PlaybackPcmSource.hpp"
namespace Pro40Audio {
// Content timing only. Re-label output frames on the already admitted RX SYT
// clock; never change packet SYT, native cutover, PCM or queue lead. Conversion
// latency is deliberately excluded. The packet replay retains RX packet shape
// and forward SYT phase (Pro40Replay.hpp:72-127), so both frame axes have the
// same rate. A new session must derive a new offset; no persisted calibration.
struct NativePlaybackTimeline {
    int64_t offsetFrames=0;
    uint64_t sampleAtCutover=0,firstElapsedBusTicks=0;
    long double unroundedOffsetFrames=0;
};
inline bool nativePlaybackTimeline(const Pro40Packets::PlaybackPacketPosition &first,
    const NativeClockSnapshot &s,const SourceFrameClock &clock,uint64_t cutover,
    NativePlaybackTimeline &out) noexcept {
    // Field interpretation cross-checked with references/linux-dice/ohci.c:
    // 1659-1667 and amdtp-stream.c:469-487. This is the same forward SYT
    // expansion validated offline by analyze_duplex_timing.py; no code copied.
    constexpr uint64_t busHz=24576000,period=4*busHz;
    if(!clock.valid() || clock.identity.generation!=s.generation || clock.identity.numer!=s.numer ||
       clock.identity.denom!=s.denom || !s.observations || s.bus.count<3 || !s.controller.originHost ||
       s.controller.firstBusTicks>=128*busHz || !std::isfinite(s.bus.slope) || s.bus.slope<=0 ||
       !std::isfinite(s.bus.intercept) || first.frame!=0 || first.frames!=8 || first.cycle>=64000 ||
       first.syt==65535 || (first.syt&4095)>=3072 || cutover>SourceFrameClock::maxExactFrame)return false;
    // The native start target retains two seconds bits; resolve modulo4s
    // against the pre-receive anchor before interpreting the8s packet cycles.
    const uint64_t delta=(uint64_t(first.cycle)*3072+period-s.controller.firstBusTicks%period)%period;
    // Existing startup evidence bound: target2s ahead plus at most512 no-data
    // cycles and the first data cycle. An ambiguous/late epoch is not admitted.
    if(delta>2*busHz+513*3072)return false;
    const uint64_t phase=((unsigned(first.syt>>12)+16-first.cycle%16)%16)*3072+(first.syt&4095);
    const auto elapsed=delta+phase;
    const long double hostPerSecond=1000000000.L*s.denom/s.numer;
    const long double txDelta=(static_cast<long double>(elapsed)/busHz-s.bus.intercept)/s.bus.slope*hostPerSecond;
    // Subtract integer origins BEFORE converting to floating point. Large
    // Mach uptimes must not lose sample precision through absolute-time floats.
    const long double inputDelta=static_cast<long double>(__int128(clock.origin)-s.controller.originHost);
    const long double ticksPerFrame=static_cast<long double>(clock.ticksNumerator)/clock.ticksDenominator;
    const long double raw=(txDelta-inputDelta)/ticksPerFrame;
    // Diagnostic admission bound, not an IEEE1394 requirement. This matches
    // the +/-1s loopback search and rejects gross epoch/mapping mistakes.
    if(!std::isfinite(raw) || std::abs(raw)>48000)return false;
    const int64_t offset=int64_t(std::round(raw));
    const __int128 sample=__int128(cutover)+offset;
    if(sample<0 || sample>clock.horizonFrames || sample>SourceFrameClock::maxExactFrame)return false;
    out={offset,uint64_t(sample),elapsed,raw};return true;
}
}
