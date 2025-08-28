//
// ASOHCIARStatus.cpp
// ASOHCI
//
// Decode AR INPUT_LAST completion status (DriverKit‑friendly RAII helper)
//

#include "ASOHCIARStatus.hpp"
#include "OHCIConstants.hpp"

AREventCode ASOHCIARStatus::ExtractEvent(uint16_t xferStatus) const
{
    // ContextControl[4:0] is the event code (OHCI 1.1 Table 3-2)
    const uint32_t evt = (uint32_t)xferStatus & kOHCI_ContextControl_evtCode_Mask;
    switch (evt) {
        case kOHCI_EvtCode_NoStatus:        return AREventCode::kNone;
        case kOHCI_EvtCode_Overrun:         return AREventCode::kOverrun;
        case kOHCI_EvtCode_DescriptorRead:  return AREventCode::kDescriptorReadErr;
        case kOHCI_EvtCode_DataRead:        return AREventCode::kDataReadErr;
        case kOHCI_EvtCode_DataWrite:       return AREventCode::kDataWriteErr;
        case kOHCI_EvtCode_BusReset:        return AREventCode::kBusReset;
        // Common controller encodings seen in practice
        case 0x0A:                          return AREventCode::kTimeout;   // evt_timeout
        case 0x0E:                          return AREventCode::kFlushed;   // evt_flushed
        default:                            return AREventCode::kUnknown;
    }
}

bool ASOHCIARStatus::IsSuccess(uint16_t xferStatus) const
{
    return ExtractEvent(xferStatus) == AREventCode::kNone;
}

const char* ASOHCIARStatus::EventString(AREventCode e) const
{
    switch (e) {
        case AREventCode::kNone:             return "none";
        case AREventCode::kLongPacket:       return "long_packet";
        case AREventCode::kOverrun:          return "overrun";
        case AREventCode::kDescriptorReadErr:return "descriptor_read_error";
        case AREventCode::kDataReadErr:      return "data_read_error";
        case AREventCode::kDataWriteErr:     return "data_write_error";
        case AREventCode::kBusReset:         return "bus_reset";
        case AREventCode::kFlushed:          return "flushed";
        case AREventCode::kTimeout:          return "timeout";
        case AREventCode::kUnknown:          return "unknown";
    }
    return "unknown";
}

