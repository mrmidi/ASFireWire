#pragma once

#include <cstdint>

namespace ASFW::Async {

/**
 * Raw event and acknowledgement codes reported in the OHCI ContextControl register.
 * Values follow IEEE 1394 Open HCI 1.1, Table 3-2.
 */
enum class OHCIEventCode : uint8_t {
    kEvtNoStatus       = 0x00,
    kEvtLongPacket     = 0x02,
    kEvtMissingAck     = 0x03,
    kEvtUnderrun       = 0x04,
    kEvtOverrun        = 0x05,
    kEvtDescriptorRead = 0x06,
    kEvtDataRead       = 0x07,
    kEvtDataWrite      = 0x08,
    kEvtBusReset       = 0x09,
    kEvtTimeout        = 0x0A,
    kEvtTcodeErr       = 0x0B,
    kEvtUnknown        = 0x0E,
    kEvtFlushed        = 0x0F,
    // OHCI ContextControl event codes (Table 3-2) for AT/AR/IT/IR
    kAckComplete       = 0x11,
    kAckPending        = 0x12,
    kAckBusyX          = 0x14,
    kAckBusyA          = 0x15,
    kAckBusyB          = 0x16,
    kAckTardy          = 0x1B,
    kAckDataError      = 0x1D,
    kAckTypeError      = 0x1E,
};

/** Human-readable name for diagnostics and logging. */
inline const char* ToString(OHCIEventCode code) {
    switch (code) {
        case OHCIEventCode::kEvtNoStatus:       return "evt_no_status";
        case OHCIEventCode::kEvtLongPacket:     return "evt_long_packet";
        case OHCIEventCode::kEvtMissingAck:     return "evt_missing_ack";
        case OHCIEventCode::kEvtUnderrun:       return "evt_underrun";
        case OHCIEventCode::kEvtOverrun:        return "evt_overrun";
        case OHCIEventCode::kEvtDescriptorRead: return "evt_descriptor_read";
        case OHCIEventCode::kEvtDataRead:       return "evt_data_read";
        case OHCIEventCode::kEvtDataWrite:      return "evt_data_write";
        case OHCIEventCode::kEvtBusReset:       return "evt_bus_reset";
        case OHCIEventCode::kEvtTimeout:        return "evt_timeout";
        case OHCIEventCode::kEvtTcodeErr:       return "evt_tcode_err";
        case OHCIEventCode::kEvtUnknown:        return "evt_unknown";
        case OHCIEventCode::kEvtFlushed:        return "evt_flushed";
        case OHCIEventCode::kAckComplete:       return "ack_complete";
        case OHCIEventCode::kAckPending:        return "ack_pending";
        case OHCIEventCode::kAckBusyX:          return "ack_busy_x";
        case OHCIEventCode::kAckBusyA:          return "ack_busy_a";
        case OHCIEventCode::kAckBusyB:          return "ack_busy_b";
        case OHCIEventCode::kAckTardy:          return "ack_tardy";
        case OHCIEventCode::kAckDataError:      return "ack_data_error";
        case OHCIEventCode::kAckTypeError:      return "ack_type_error";
    }
    return "unknown_event_code";
}

} // namespace ASFW::Async

/*
 * OHCI Specification Reference:
 * - ContextControl event_code field definition: Section 3.1.1.
 * - Packet event codes: Table 3-2 (pages 18-19).
 */

