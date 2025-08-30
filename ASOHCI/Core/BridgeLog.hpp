// BridgeLog.hpp
// Lightweight ring buffer logging bridge for DriverKit

#pragma once

#include <DriverKit/IOLib.h>
#include <DriverKit/OSData.h>

// Initialize logging ring (idempotent)
void bridge_log_init();

// Variadic log append (printf-like)
void bridge_logf(const char *fmt, ...);

// Convenience macro (disabled: route logs via os_log only)
#define BRIDGE_LOG(fmt, ...)                                                   \
  do {                                                                         \
  } while (0)

// Copy recent log lines into an OSData blob
kern_return_t bridge_log_copy(OSData **outData);
