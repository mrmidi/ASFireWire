#pragma once
#include <cstdint>
#include "../Common/FWCommon.hpp"  // Single source of truth for all constants

// ============================================================================
// ROM Reader Configuration
// ============================================================================
// If READ_MODE_QUAD is defined, ROM reader will use quadlet reads only
// (no block reads). Config ROM MUST be read quadlet-by-quadlet per IEEE 1394 spec.
#ifndef READ_MODE_QUAD
#define READ_MODE_QUAD 1  // REQUIRED: Config ROM must use quadlet reads, not block reads
#endif

namespace ASFW::Discovery {

// ============================================================================
// IEEE 1394-1995 Speed Codes
// ============================================================================
// Single source of truth: FW::Speed in FWCommon.hpp
using FwSpeed = ::ASFW::FW::FwSpeed;

// ============================================================================
// Config ROM CSR Addresses
// ============================================================================
// Single source of truth: FW::ConfigROMAddr in FWCommon.hpp
namespace ConfigROMAddr = ::ASFW::FW::ConfigROMAddr;

// ============================================================================
// Config ROM Directory Entry Types
// ============================================================================
// Single source of truth: FW::EntryType in FWCommon.hpp
namespace EntryType = ::ASFW::FW::EntryType;

// ============================================================================
// Config ROM Directory Keys
// ============================================================================
// Single source of truth: FW::ConfigKey in FWCommon.hpp
namespace ConfigKey = ::ASFW::FW::ConfigKey;

// ============================================================================
// Config ROM Header + Bus Options Fields (IEEE 1212 / TA 1999027)
// ============================================================================
// Single source of truth: FWCommon.hpp
namespace ConfigROMHeaderFields = ::ASFW::FW::ConfigROMHeaderFields;
namespace BusOptionsFields = ::ASFW::FW::BusOptionsFields;

// ============================================================================
// Max Payload by Speed (Conservative Values)
// ============================================================================
// Single source of truth: FW::MaxPayload in FWCommon.hpp
namespace MaxPayload = ::ASFW::FW::MaxPayload;

} // namespace ASFW::Discovery
