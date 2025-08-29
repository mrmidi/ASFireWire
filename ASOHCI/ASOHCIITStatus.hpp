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
    // Symbolic (provisional) status codes – actual controller may differ; centralize so future
    // hardware-specific port can override via table.
    static constexpr uint16_t kStatus_OK0        = 0x00; // success variant 0
    static constexpr uint16_t kStatus_OK1        = 0x01; // success variant 1 (some controllers use multiple OK codes)
    static constexpr uint16_t kStatus_UNDERRUN0  = 0x04;
    static constexpr uint16_t kStatus_UNDERRUN1  = 0x05;
    static constexpr uint16_t kStatus_LATE0      = 0x06;
    static constexpr uint16_t kStatus_SKIPPED0   = 0x07;
    static constexpr uint16_t kStatus_FATAL0     = 0x0F;

    ITCompletion Decode(uint16_t xferStatus, uint16_t timeStamp) const {
        ITCompletion c{};
        c.timeStamp = timeStamp;
        uint16_t code = xferStatus & 0x1F; // limit to low bits (typical event field width)
        switch (code) {
            case kStatus_OK0:
            case kStatus_OK1:
                c.success = true; c.event = ITEvent::kNone; break;
            case kStatus_UNDERRUN0:
            case kStatus_UNDERRUN1:
                c.success = false; c.event = ITEvent::kUnderrun; break;
            case kStatus_LATE0:
                c.success = false; c.event = ITEvent::kLate; break;
            case kStatus_SKIPPED0:
                c.success = false; c.event = ITEvent::kSkipped; break;
            case kStatus_FATAL0:
                c.success = false; c.event = ITEvent::kUnrecoverable; break;
            default:
                // Heuristic fallback: treat unknown non-zero codes < 0x10 as late vs bigger as unrecoverable.
                if (code && code < 0x10) {
                    c.success = false; c.event = ITEvent::kLate; // safest to re-arm; not fatal
                } else {
                    c.success = false; c.event = ITEvent::kUnknown;
                }
                break;
        }
        return c;
    }
};

