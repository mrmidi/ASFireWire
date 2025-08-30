#pragma once
//
// FWCodes.hpp
// Ack and response code enums used by ASFireWire command/completion paths.
// Values mirror IEEE 1394 semantics and legacy IOFireWire usage for
// compatibility, but this header is project-local for DriverKit.
//

// FireWire-specific IOReturn codes
enum {
  kIOFireWirePending =
      0xE0008002, // Command is queued/pending (from legacy IOFireWireFamily)
  kIOFireWireResponseBase =
      0xE0008000 // Base for response codes (from legacy IOFireWireFamily)
};

// Link-layer acknowledge codes (and local pseudo-acks)
// Note: kFWAckTimeout is a locally generated pseudo-ack indicating timeout.
enum FWAck : int {
  kFWAckTimeout = -1,
  kFWAckComplete = 1,
  kFWAckPending = 2,
  kFWAckBusyX = 4,
  kFWAckBusyA = 5,
  kFWAckBusyB = 6,
  kFWAckDataError = 13,
  kFWAckTypeError = 14
};

// Transaction response codes (including locally generated pseudo-responses)
// Values align with standard rcode meanings used in async transactions.
enum FWResponse : int {
  kFWResponseComplete = 0,       // OK
  kFWResponseConflictError = 4,  // Resource conflict; may retry
  kFWResponseDataError = 5,      // Data not available / CRC error
  kFWResponseTypeError = 6,      // Operation not supported
  kFWResponseAddressError = 7,   // Invalid destination address
  kFWResponseBusResetError = 16, // Local pseudo-response after bus reset
  kFWResponsePending = 17        // Local pseudo: real response later
};
