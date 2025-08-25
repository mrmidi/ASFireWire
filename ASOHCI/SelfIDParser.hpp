// SelfIDParser.hpp
// Self-ID quadlet parsing and debug logging

#pragma once

#include <stdint.h>

namespace SelfIDParser {

// Parse and log Self-ID packets from a quadlet buffer.
// Expects 'quadletCount' 32-bit words in 'selfIDData'.
void Process(uint32_t* selfIDData, uint32_t quadletCount);

}

