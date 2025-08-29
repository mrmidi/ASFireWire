#pragma once
//
// ASOHCIITStatus.hpp
// Interprets IT completion state from OUTPUT_LAST* xferStatus/timestamp.
// (Isochronous has no IEEE1394 ACK codes; we synthesize late/underrun/skip events.)
//
// Spec refs (OHCI 1.1): §9.5 (interrupt causes incl. underrun), §9.6 (timestamp fields), Chapter 6 (event capture)

#include <stdint.h>

enum class ITEvent : uint8_t {
    kNone        = 0,
    kUnderrun,           // transmitter starved (§9.5)
    kLate,               // cycle late, packet dropped or skipped (policy) (§9.5)
    kSkipped,            // skipped by program flow (§9.4 appending semantics)
    kUnrecoverable,      // dead/timeout (skip overflow) (§9.5)
    kUnknown
};

struct ITCompletion {
    bool      success = false;   // transmitted in time this cycle
    ITEvent   event   = ITEvent::kNone;
    uint16_t  timeStamp = 0;     // if provided by controller (§9.6)
};

class ASOHCIITStatus {
public:
    ITCompletion Decode(uint16_t xferStatus, uint16_t timeStamp) const {
        ITCompletion c{};
        c.timeStamp = timeStamp;
        // Placeholder mapping (spec §9.5): real codes controller-specific; use heuristic ranges.
        switch (xferStatus & 0x1F) { // low 5 bits often carry event/reason
            case 0x00: // nominal success
                c.success = true; c.event = ITEvent::kNone; break;
            case 0x04: // underrun example
            case 0x05:
                c.success = false; c.event = ITEvent::kUnderrun; break;
            case 0x06: // late
                c.success = false; c.event = ITEvent::kLate; break;
            case 0x07: // skipped
                c.success = false; c.event = ITEvent::kSkipped; break;
            case 0x0F: // fatal/unrecoverable
                c.success = false; c.event = ITEvent::kUnrecoverable; break;
            default:
                c.success = false; c.event = ITEvent::kUnknown; break;
        }
        return c;
    }
};

