// LogHelper.hpp
// Shared logging helper for DriverKit and user-space builds

#pragma once

#include <TargetConditionals.h>
#include <cstddef>
#include <cstdint>
#include <os/log.h>

static inline os_log_t ASLog() {
#if defined(TARGET_OS_DRIVERKIT) && TARGET_OS_DRIVERKIT
  return OS_LOG_DEFAULT;
#else
  static os_log_t log = os_log_create("net.mrmidi.ASFireWire", "ASOHCI");
  return log;
#endif
}

// Dump a memory region as hex lines to os_log (big-endian view).
// Keeps output concise by trimming trailing zeroes but prints at least 64 bytes
// and rounds up to 16-byte lines for readability.
inline char ASHexDigit(uint8_t v) {
  return (v < 10) ? char('0' + v) : char('a' + (v - 10));
}

inline void DumpHexBigEndian(const void *data, size_t length,
                             const char *title) {
  if (!data || length == 0)
    return;
  const uint8_t *p = static_cast<const uint8_t *>(data);

  // Determine effective length by trimming trailing zeros; keep at least 64
  // bytes
  size_t eff = length;
  while (eff > 0 && p[eff - 1] == 0) {
    --eff;
  }
  const size_t kMinDump = 64; // header + small root dir
  if (eff < kMinDump)
    eff = (length < kMinDump) ? length : kMinDump;
  // round up to 16-byte boundary for clean lines
  if (eff % 16) {
    size_t rounded = ((eff + 15) / 16) * 16;
    eff = (rounded <= length) ? rounded : length;
  }

  os_log(ASLog(), "ASOHCI: === %{public}s (BIG-ENDIAN) === size=%lu dump=%lu",
         title ? title : "DUMP", (unsigned long)length, (unsigned long)eff);

  char line[80];
  for (size_t off = 0; off < eff; off += 16) {
    size_t pos = 0;
    // 4 hex digits of offset (16-bit is enough for our capped dumps)
    uint16_t o = static_cast<uint16_t>(off);
    line[pos++] = ASHexDigit(uint8_t((o >> 12) & 0xF));
    line[pos++] = ASHexDigit(uint8_t((o >> 8) & 0xF));
    line[pos++] = ASHexDigit(uint8_t((o >> 4) & 0xF));
    line[pos++] = ASHexDigit(uint8_t(o & 0xF));
    line[pos++] = ':';
    // up to 16 bytes
    for (size_t i = 0; i < 16 && (off + i) < length; ++i) {
      uint8_t b = p[off + i];
      line[pos++] = ' ';
      line[pos++] = ASHexDigit(uint8_t((b >> 4) & 0xF));
      line[pos++] = ASHexDigit(uint8_t(b & 0xF));
    }
    line[pos] = '\0';
    os_log(ASLog(), "ASOHCI: %{public}s", line);
  }
  os_log(ASLog(), "ASOHCI: === END OF DUMP ===");
}
