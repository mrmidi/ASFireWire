// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "ExistingClockCheck.hpp"
#include <cstdint>
#include <initializer_list>

namespace Pro40 {
// Diagnostic native-startup option, not exposed by the worker CLI. Rate encoding and changed-
// rate acceptance follow Linux dice-stream.c:60-99 and ClockPolicy.hpp. Neither
// reference prescribes an excursion as a cure. This tests a distinct hypothesis.
enum class ClockExcursionPhase { admission, to44100, to48000, complete };
enum class ClockExcursionError { none, cancelled, deadline, io, mismatch };
struct ClockExcursionResult {
    ClockExcursionPhase phase=ClockExcursionPhase::admission;
    ClockExcursionError error=ClockExcursionError::none;
    int32_t io=0;
    unsigned rateAttempts=0;
    // Even a failed write ACK may have changed hardware. Caller must retain
    // the owned session and run guarded restoration/cleanup before releasing it.
    bool restorationRequired=false;
};
// Backend contract: guard checks the original generation/lease; read returns a
// fresh state; selectAndVerify performs ONE rate write, requires acceptance and
// stable locked readback, and has its own bounded IO/wait. nowMicros is monotonic.
// No stream/channel/ENABLE operation exists here. Caller must not add a third
// redundant 48k write. Cancellation prevents further experimental transitions;
// it must NOT suppress the caller's separate guarded cleanup restoration.
template<class Backend>
[[nodiscard]] ClockExcursionResult clockExcursion48k(Backend &io,uint64_t owner) {
    ClockExcursionResult result;
    const auto began=io.nowMicros();
    auto gate=[&] {
        if(io.cancelled()){result.error=ClockExcursionError::cancelled;return false;}
        const auto now=io.nowMicros();
        if(now<began || now-began>=7000000){result.error=ClockExcursionError::deadline;return false;}
        return true;
    };
    auto checked=[&](auto work) {
        if(!gate())return false;
        result.io=work();
        if(result.io){result.error=ClockExcursionError::io;return false;}
        return gate();
    };
    auto observe=[&](uint32_t hz) {
        if(!checked([&]{return io.guard();}))return false;
        ExistingClockSample sample{};
        if(!checked([&]{return io.read(sample);}))return false;
        if(!checked([&]{return io.guard();}))return false;
        const auto index=hz==44100?1u:2u;
        if(owner==0xffff000000000000ULL || sample.owner!=owner || sample.enabled ||
            sample.selected!=((index<<8)|12) || sample.status!=((index<<8)|1) ||
            sample.measured!=hz || (sample.capabilities&6)!=6){
            result.error=ClockExcursionError::mismatch;return false;
        }
        return true;
    };
    if(!observe(48000))return result;
    for(uint32_t hz:{44100u,48000u}) {
        result.phase=hz==44100?ClockExcursionPhase::to44100:ClockExcursionPhase::to48000;
        if(!checked([&]{return io.guard();}) || !gate())return result;
        if(!checked([&]{
            result.restorationRequired=true;
            ++result.rateAttempts;
            return io.selectAndVerify(hz);
        }) || !observe(hz))return result;
    }
    result.phase=ClockExcursionPhase::complete;
    return result;
}
}
