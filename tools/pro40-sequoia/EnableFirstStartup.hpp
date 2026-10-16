// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <array>
#include <cstdint>

namespace Pro40 {
// Diagnostic sequence used by the native runner; policy version 2.
// Behavioral evidence: reference-host/VENDOR_STARTUP_STATIC_REVIEW.md, second
// pass (vendor ENABLE at 0xff48, host TX/RX starts at 0xfffc/0x1007d);
// Linux references/linux-dice/dice-stream.c:430-455 enables before domain start.
// No vendor code is used. A vendor delay is deliberately not assumed necessary.
//
// Caller owns all resources, preconfigures channels, and waits for admission
// BEFORE calling. Backend enableDevice must use Pro40StreamEnable so even a
// failed write ACK is cleared. startGuard must check cancellation, generation,
// ownership/clock policy and remaining scheduled lead with ENABLE now set.
// Guard after every start as well: a blocking Start can cross the deadline.
// Every result, including success, requires the caller's normal stop-all/clear
// cleanup. Never retry, reschedule or fall back to another startup sequence.
// RX starts immediately, unlike scheduled TX. Keep its queue exposure bounded.
// receiveGuard revalidates state, cancellation and the monotonic deadline on
// every iteration; pumpReceiveWait must return promptly. No target rescheduling.
template<class Backend>
[[nodiscard]] int32_t waitForEnableFirstReceive(Backend &io) {
    for(;;) {
        uint32_t lead=0;
        const auto status=io.receiveGuard(lead);
        if(status)return status;
        if(lead<=512)return 0;
        io.pumpReceiveWait();
    }
}
template<class Backend>
[[nodiscard]] int32_t enableFirstStartup(Backend &io) {
    auto status=io.enableDevice();
    if(status)return status;
    constexpr std::array<unsigned,4> order{2,3,0,1}; // host TX, then host RX
    for(const auto index:order) {
        if(index==0) {
            status=waitForEnableFirstReceive(io);
            if(status)return status;
        }
        status=io.startGuard(index);
        if(status)return status;
        status=io.startHost(index);
        if(status)return status;
        status=io.startGuard(index);
        if(status)return status;
    }
    return 0;
}
}
