//
// IsochTypes.hpp
// ASFWDriver
//
// Core Isochronous Type Definitions and OHCI context definitions.
//

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <TargetConditionals.h>

namespace ASFW::Isoch {

// Callback for received packets (Raw transport level)
// @param data: Span containing packet data (header + payload)
// @param status: Status bits from descriptor
// @param timestamp: Timestamp of reception
using IsochReceiveCallback = std::function<void(std::span<const uint8_t> data, uint32_t status, uint64_t timestamp)>;

} // namespace ASFW::Isoch
