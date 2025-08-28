//
// ASOHCIATStatus.cpp
// ASOHCI
//
// OHCI 1.1 AT Status helpers (namespace implementation)
// Spec refs: §7.1.5.2 (xferStatus), §7.6 (AT interrupts)
//

#include "ASOHCIATStatus.hpp"

using namespace ATStatus;

// Internal helpers to decode fields from a 16-bit xferStatus view
static inline uint8_t _AckBits(uint16_t xferStatus) {
    return static_cast<uint8_t>((xferStatus >> 8) & 0x0F);
}

static inline ATEvent _EventFromXfer(uint16_t xferStatus) {
    // Crude event bucketing from high nibble of xferStatus lower 16 bits.
    // Controllers differ; callers treat this as best-effort.
    uint8_t evt = static_cast<uint8_t>((xferStatus >> 12) & 0x0F);
    switch (evt) {
        case 0x0: return ATEvent::kUnknown;      // no explicit event encoded here
        case 0x2: return ATEvent::kMissingAck;
        case 0x3: return ATEvent::kUnderrun;
        case 0x4: return ATEvent::kUnknown;      // overrun not typical on AT
        case 0x6: return ATEvent::kDataRead;
        case 0x7: return ATEvent::kUnknown;      // data write (builder specific)
        case 0x8: return ATEvent::kUnknown;      // bus reset handled elsewhere
        case 0x9: return ATEvent::kTimeout;
        case 0xE: return ATEvent::kFlushed;
        default:  return ATEvent::kUnknown;
    }
}

Completion ATStatus::Decode(const HWStatusWord& s)
{
    Completion c{};
    uint16_t xfer = static_cast<uint16_t>(s.raw & 0xFFFFu);

    // Map ACK
    switch (_AckBits(xfer)) {
        case 0x0: c.ack = ATAck::kComplete; break;
        case 0x1: c.ack = ATAck::kPending;  break;
        case 0x2: case 0x3: case 0x4: c.ack = ATAck::kBusy; break;
        case 0x6: c.ack = ATAck::kDataError; break;
        default:  c.ack = ATAck::kUnknown; break;
    }

    // Map event (best-effort from available bits)
    c.event = _EventFromXfer(xfer);
    c.details = s.raw; // retain raw for further parsing if needed
    return c;
}

ATAck ATStatus::ToAck(const Completion& c)
{
    return c.ack;
}

bool ATStatus::IsSuccess(const Completion& c)
{
    // Success if ACK is complete or pending; ignore event unless it indicates timeout/flush
    bool ackOK = (c.ack == ATAck::kComplete || c.ack == ATAck::kPending);
    if (!ackOK) return false;
    if (c.event == ATEvent::kTimeout || c.event == ATEvent::kFlushed) return false;
    return true;
}
