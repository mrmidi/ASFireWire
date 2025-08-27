// ASOHCIInterruptDump.hpp
// Helper for decoding and logging OHCI IntEvent bits

#pragma once

#include <stdint.h>

namespace LogUtils {
void DumpIntEvent(uint32_t ev);
}

