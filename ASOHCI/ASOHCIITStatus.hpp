#pragma once
//
// ASOHCIITStatus.hpp
// Interprets IT completion state from OUTPUT_LAST* xferStatus/timestamp.
// (Isochronous has no IEEE1394 ACK codes; we report late/underrun/skip/etc.)
//
// Spec refs: OHCI 1.1 IT §6.3 (interrupts), §6.4 (status/timestamp fields)

#include <stdint.h>

enum class ITEvent : uint8_t {
    kNone        = 0,
    kUnderrun,           // transmitter starved (§6.3/§6.4)
    kLate,               // cycle late, packet dropped or skipped (policy) (§6.4)
    kSkipped,            // skipped by program flow (§6.1 appending/branch)
    kUnrecoverable,      // dead/timeout (skip overflow) (§6.3)
    kUnknown
};

struct ITCompletion {
    bool      success = false;   // transmitted in time this cycle
    ITEvent   event   = ITEvent::kNone;
    uint16_t  timeStamp = 0;     // if provided by controller (§6.4)
};

class ASOHCIITStatus {
public:
    ITCompletion Decode(uint16_t xferStatus, uint16_t timeStamp) const;
};

