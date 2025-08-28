//#pragma once
//
// ASOHCIATStatus.hpp
// Event/ACK mapping helpers (headers only)
//
// Spec refs: OHCI 1.1 §7.5 (Interrupts & completion), §7.2 (ack_data_error vs underrun), §7.6 (flush/missing)

#pragma once

#include "ASOHCIATTypes.hpp"

namespace ATStatus {

// Opaque hardware status word captured from OUTPUT_LAST* completion (§7.5)
struct HWStatusWord {
    uint32_t raw = 0;
};

// Parsed completion (driver-facing)
struct Completion {
    ATEvent event = ATEvent::kUnknown;
    ATAck   ack   = ATAck::kUnknown;
    uint32_t details = 0;     // optional: retry counts, tCode, etc.
};

// Decode a HW status word (no implementation here)
Completion Decode(const HWStatusWord& s);            // §7.5, §7.2
ATAck      ToAck(const Completion& c);               // §7.5
bool       IsSuccess(const Completion& c);           // complete/pending (§7.5)

} // namespace ATStatus

