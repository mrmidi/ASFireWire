#pragma once
//
// ASOHCIITStatus.hpp
// Interprets IT completion state from OUTPUT_LAST* xferStatus/timestamp.
// (Isochronous has no IEEE1394 ACK codes; we synthesize late/underrun/skip events.)
//
// Spec refs (OHCI 1.1): §9.5 (interrupt causes incl. underrun), §9.6 (timestamp fields), Chapter 6 (event capture)

#include <stdint.h>

enum class ITEvent : uint8_t {
    kNone = 0,
    kUnderrun,       // transmitter starved (§9.5)
    kLate,           // packet missed its target cycle (§9.5)
    kSkipped,        // program logic skipped (padding / chain) (§9.4)
    kUnrecoverable,  // dead/timeout / internal error (§9.5)
    kUnknown
};

struct ITCompletion {
    bool      success = false;   // transmitted in time this cycle
    ITEvent   event   = ITEvent::kNone;
    uint16_t  timeStamp = 0;     // if provided by controller (§9.6)
};

class ASOHCIITStatus {
public:
    ITCompletion Decode(uint16_t xferStatus, uint16_t timeStamp) const;
};

