// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "SustainedClockPublication.hpp"
namespace Pro40Audio {
// A clock snapshot names an earlier consumed prefix than the latest published
// PCM batch. Retain startup history plus batched delivery slack.
// In session-d8esq7x2: prefix4600, published5632, requested first1528;
// a4096-frame window had already discarded through1535. Increasing storage
// does not change inputFirst, the clock model, or the startup latency policy.
inline constexpr size_t nativeDuplexInputWindowFrames=8192;
// Fresh input is published independently of clock reports. Conditional model:
// 64-cycle input groups,512-frame publication,20ms native delay,10ms IPC delay,
// 20ms callback delay and64..512-frame requests.2560 fails at some startup
// phases;2688 passes the phase sweep.2816 retains128 additional frames of slack.
// These bounds are assumptions, not scheduler guarantees or HAL safety values.
inline constexpr uint64_t nativeDuplexInputHistoryFrames=2816;
// Content-side startup margins, in device frames. The first application frame
// follows both the native preparation lead and the admitted silent prime.
// The4096-frame lead retains time for admission; a1024-frame silent prime
// reduces the combined offset from128ms to106.667ms at48kHz. Keep storage,
// input history and FireWire bank/SYT geometry separate from this policy.
// Conditional model:40ms admission, independent20ms callback/native delays,
// 10ms delivery,256-cycle native batches. A512-frame prime underruns under
// these bounds. They are test assumptions, not scheduler guarantees. Silence
// still uses the existing AM824 encoder (linux-dice/amdtp-am824.c:209-221).
inline constexpr uint64_t nativeDuplexLeadFrames=4096;
inline constexpr uint64_t nativeDuplexPrimeFrames=1024;
// Same policy on both ends of this experimental duplex connection. The age of
// a consumed receive prefix is distinct from clock-message arrival/use age and
// from analogue latency. NativeClockFit already bounds that prefix to 128 ms.
// A separate 64-ms limit rejected valid 512-descriptor hardware delivery:
// 66.618 ms in session-nekgzhk9, 68.475 ms in the prior successful 90-second run.
// Keep the decoder's bound, packet continuity, rates and publication guards.
inline SustainedClockPolicy nativeDuplexClockPolicy(uint32_t numer,uint32_t denom) noexcept {
    if(!numer || !denom)return {};
    const auto ticks=[&](uint64_t ns){return uint64_t(__uint128_t(ns)*denom/numer);};
    return {4096,128,8,48000,ticks(250000000),ticks(500000000),ticks(200000),ticks(1000000000),
        NativeClockFit::maximumPacketAgeTicks,500,100};
}
}
