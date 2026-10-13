// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <CoreFoundation/CoreFoundation.h>
namespace NativePackets {
// Only armed around the outer streaming dispatcher, never during native Stop
// or deferred finalization. CFRunLoopStop ends the current activation, not DMA.
// Apple CFRunLoopStop documentation: only the innermost activation is exited.
// https://developer.apple.com/documentation/corefoundation/cfrunloopstop(_:)
class DispatchBoundary {
    bool active_=false;
    unsigned yields_=0;
public:
    void setActive(bool active) {active_=active;}
    void completed(unsigned count) {
        if(active_ && count && count%128==0) {
            ++yields_;
            CFRunLoopStop(CFRunLoopGetCurrent());
        }
    }
    void transmitCompleted(unsigned count,unsigned bank=128) {
        // Same boundary+1 as transmitBankReady. Completion and cycle-timer
        // observations are independent; EarlyTransmitGate still checks the one/two
        // cycle early cases. Bank-end and64/32-cycle guards remain authoritative.
        if(active_ && (bank==128 || bank==256) && count>bank && count%bank==1) {
            ++yields_;
            CFRunLoopStop(CFRunLoopGetCurrent());
        }
    }
    unsigned yields() const {return yields_;}
    bool active() const {return active_;}
};
}
