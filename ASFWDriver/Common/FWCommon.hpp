#pragma once

// FWCommon.hpp — Umbrella include for all FireWire common definitions.
//
// Maintains backward compatibility: all existing #include "FWCommon.hpp"
// continue to work unchanged. Files that only need one concern may switch
// to the focused header.
//
// Split layout:
//   WireFormat.hpp  — Bit manipulation + big-endian byte-array I/O
//   FWTypes.hpp     — Protocol enums (Ack, Response, Speed) + strong types
//   CSRSpace.hpp    — CSR addresses + Config ROM + bus options + validation
//
// These are also kept here for headers that include FWCommon.hpp and rely on
// IOReturn or the FWAddress forward declaration.
#include <DriverKit/IOReturn.h>
#include "ASFWIOReturn.hpp"

#include "WireFormat.hpp"
#include "FWTypes.hpp"
#include "CSRSpace.hpp"
