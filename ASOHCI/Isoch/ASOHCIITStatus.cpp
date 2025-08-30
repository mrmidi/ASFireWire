// ASOHCIITStatus.cpp
#include "ASOHCIITStatus.hpp"
#include "OHCIConstants.hpp"

ITCompletion ASOHCIITStatus::Decode(uint16_t xferStatus,
                                    uint16_t timeStamp) const {
  ITCompletion c{};
  c.timeStamp = timeStamp;

  const uint16_t evt = (xferStatus & kOHCI_ContextControl_evtCode_Mask);
  switch (evt) {
  case kOHCI_EvtCode_NoStatus:
  case kOHCI_EvtCode_AckComplete:
    c.success = true;
    c.event = ITEvent::kNone;
    break;
  case kOHCI_EvtCode_Underrun:
    c.event = ITEvent::kUnderrun;
    break;
  case kOHCI_EvtCode_BusReset:
    c.event = ITEvent::kUnrecoverable;
    break;
  default:
    c.event = ITEvent::kUnknown;
    break;
  }
  return c;
}
