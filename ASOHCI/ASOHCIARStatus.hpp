#pragma once
//
// ASOHCIARStatus.hpp
// Decode AR INPUT_LAST completion status
//
// Spec refs: OHCI 1.1 §8.1.5 (status/timestamp in INPUT_LAST), §8.6 (AR
// interrupts)

#include "ASOHCIARTypes.hpp"
#include <DriverKit/IOReturn.h>
#include <stdint.h>

enum class AREventCode : uint8_t {
  kNone,
  kLongPacket,
  kOverrun,
  kDescriptorReadErr,
  kDataReadErr,
  kDataWriteErr,
  kBusReset,
  kFlushed,
  kTimeout,
  kUnknown
};

// RAII helper (plain C++)
class ASOHCIARStatus {
public:
  ASOHCIARStatus() = default;
  ~ASOHCIARStatus() = default;

  kern_return_t Initialize() { return kIOReturnSuccess; }

  // Map 16-bit xferStatus into an AREventCode (no ACK on AR path)
  AREventCode ExtractEvent(uint16_t xferStatus) const;

  // Success on AR generally means “no error event” (§8.6)
  bool IsSuccess(uint16_t xferStatus) const;

  const char *EventString(AREventCode e) const;
};
