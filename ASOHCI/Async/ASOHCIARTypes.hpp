#pragma once
//
// ASOHCIARTypes.hpp
// AR-side small types & enums
//
// Spec refs: OHCI 1.1 §8.1 (AR programs), §8.2 (AR context regs),
//            §8.4 (buffer-fill), §8.6 (interrupts), §8.7 (data formats)

#include <stdint.h>

enum class ARContextRole : uint8_t {
  kRequest, // receives asynchronous requests
  kResponse // receives asynchronous responses
};

enum class ARBufferFillMode : uint8_t {
  kImmediate, // deliver each packet as soon as it completes (§8.4)
  kBufferFill // allow HW to pack multiple frames into a buffer (§8.4)
};

enum class ARInterruptKind : uint8_t {
  kPacketArrived,
  kBufferComplete,
  kOverrun,
  kDropped,
  kBusReset,
  kOther
};

// IEEE 1394 TCodes of interest on AR (subset for async; isoch excluded)
enum class ARTCode : uint8_t {
  kWriteQuadlet = 0x00,
  kWriteBlock = 0x01,
  kWriteResponse = 0x02,
  kReadQuadlet = 0x04,
  kReadBlock = 0x05,
  kReadResponse = 0x06,
  kCycleStart = 0x0C, // appears on AR only if PHY/IRMs route it
  kLock = 0x07,       // lock request/response (various)
  kUnknown = 0xFF
};

// Optional hardware-side filtering knobs we expose at init time
struct ARFilterOptions {
  bool filterPhysicalReadsAndIsoch =
      true; // route physical reads/isoch away (§8.6 hint)
  bool acceptPhyPackets =
      false;                   // enable PHY packet capture if supported (§8.*)
  bool acceptBroadcast = true; // permit bcast requests
  uint16_t nodeIDPhysicalFilter = 0xFFFF; // if supported by link ctrl
};

// Lightweight view of a received packet within a buffer
struct ARPacketView {
  const uint8_t *data = nullptr; // CPU VA into RX buffer
  uint32_t length = 0;           // bytes valid
  uint16_t timeStamp = 0;        // from INPUT_LAST status quadlet (§8.1.5)
  uint16_t xferStatus = 0;       // from INPUT_LAST status quadlet (§8.1.5)
};
