#pragma once

#include <cstdint>
#include "RegisterMap.hpp"

namespace ASFW::Driver {

// ============================================================================
// OHCI Register Constants (shared across subsystems)
// ============================================================================

// AR Filter Constants (OHCI §7.4)
// Bit 31 in AsReqFilterHiSet = accept all async requests
constexpr uint32_t kAsReqAcceptAllMask = 0x80000000u;

// Default link control configuration used during controller initialization
constexpr uint32_t kDefaultLinkControl = LinkControlBits::kRcvSelfID |
										   LinkControlBits::kRcvPhyPkt |
										   LinkControlBits::kCycleTimerEnable;

// Posted write priming bits (OHCI HCControl - enable posted writes and LPS)
constexpr uint32_t kPostedWritePrimingBits = HCControlBits::kPostedWriteEnable |
											 HCControlBits::kLPS;

// Default ATRetries value (cycleLimit=200 maxPhys=3 maxResp=3 maxReq=3)
constexpr uint32_t kDefaultATRetries = (3u << 0) | (3u << 4) | (3u << 8) | (200u << 16);

// Default node capabilities for our local node (kNodeCapabilities: general device flag set)
constexpr uint32_t kDefaultNodeCapabilities = 0x00000001u;

// OHCI version check for 1.1 (0x010010) used in initial channel configuration
constexpr uint32_t kOHCI_1_1 = 0x010010u;

// Soft reset timeouts used by controller reset sequences
constexpr uint32_t kSoftResetTimeoutUsec = 500'000u; // 500ms
constexpr uint32_t kSoftResetPollUsec = 1'000u;      // 1ms

// ============================================================================
// DMA Context Control Bit Positions (OHCI §7.2.3.2)
// ============================================================================
//
// OHCI Context Control Register Bit Layout
// Verified against:
//   - Linux firewire/ohci.c:247-250
//   - Apple AppleFWOHCI (IDA analysis)
//   - OHCI 1.1 Specification §7.2.3.2
//
// ControlSet/ControlClear Register (write):
//   Bit 15: RUN    - Start/continue DMA program execution
//   Bit 12: WAKE   - Signal that new descriptors are available (edge-triggered)
//   Bit 11: DEAD   - Context encountered unrecoverable error
//   Bit 10: ACTIVE - DMA engine is currently processing descriptors
//   Bits 4-0: Event code (for error/completion status)
//
// ControlSet Register (read-back):
//   Bit 15: run    - Context is armed and will process descriptors
//   Bit 12: wake   - (transient) Clears after DMA engine acknowledges
//   Bit 11: dead   - Fatal error flag (requires context reset)
//   Bit 10: active - Hardware is actively fetching/executing descriptors
//
// Usage Pattern (from Linux context_run/context_append):
//   PATH 1 (first packet): Write CommandPtr, then ControlSet = RUN (0x8000)
//   PATH 2 (chained packets): Update branch, then ControlSet = WAKE (0x1000)
//
// Reference:
//   Linux: #define CONTEXT_RUN 0x8000, CONTEXT_WAKE 0x1000, CONTEXT_DEAD 0x0800, CONTEXT_ACTIVE 0x0400
//   Apple: WriteControlSet(RUN|WAKE) logs as 0x9000 = 0x8000 | 0x1000

constexpr uint32_t kContextControlRunBit    = 0x00008000; // Bit 15 (RUN)
constexpr uint32_t kContextControlWakeBit   = 0x00001000; // Bit 12 (WAKE) - FIXED from 0x0400
constexpr uint32_t kContextControlDeadBit   = 0x00000800; // Bit 11 (DEAD)
constexpr uint32_t kContextControlActiveBit = 0x00000400; // Bit 10 (ACTIVE) - FIXED from 0x0200
constexpr uint32_t kContextControlEventMask = 0x0000001F; // Bits 4-0 (event code)

// Compile-time validation: verify bit positions match Linux/OHCI spec
static_assert(kContextControlRunBit    == 0x8000, "RUN bit must be bit 15 (0x8000)");
static_assert(kContextControlWakeBit   == 0x1000, "WAKE bit must be bit 12 (0x1000)");
static_assert(kContextControlDeadBit   == 0x0800, "DEAD bit must be bit 11 (0x0800)");
static_assert(kContextControlActiveBit == 0x0400, "ACTIVE bit must be bit 10 (0x0400)");

// Verify non-overlapping (paranoid check)
static_assert((kContextControlRunBit & kContextControlWakeBit) == 0, "Bit overlap detected");
static_assert((kContextControlRunBit & kContextControlDeadBit) == 0, "Bit overlap detected");
static_assert((kContextControlRunBit & kContextControlActiveBit) == 0, "Bit overlap detected");

// ContextControl struct for cleaner call sites (matches IsochReceiveContext usage)
struct ContextControl {
    static constexpr uint32_t kRun = kContextControlRunBit;
    static constexpr uint32_t kWake = kContextControlWakeBit;
    static constexpr uint32_t kDead = kContextControlDeadBit;
    static constexpr uint32_t kActive = kContextControlActiveBit;
    static constexpr uint32_t kEventCodeMask = kContextControlEventMask;
    static constexpr uint32_t kEventCodeShift = 0;
    static constexpr uint32_t kIsochHeader = 1u << 30;     // IR: includes isoch header (OHCI §10.2.2)
    static constexpr uint32_t kCycleMatchEnable = 1u << 30; // IT: stall until cycle match (OHCI §9.2)
    // Mask of all writable bits (for safe clearing without hitting reserved bits)
    static constexpr uint32_t kWritableBits = kRun | kWake | kCycleMatchEnable;
};

// ============================================================================
// IEEE 1394 Wire Format Constants - Asynchronous Packet Headers
// ============================================================================
//
// CRITICAL DISTINCTION:
// - OHCI Internal Format: Used in some OHCI registers, has fields like
//   srcBusID, speed code - NOT for immediateData[]
// - IEEE 1394 Wire Format (below): Standard packet format transmitted on the
//   bus - THIS is what goes into descriptor immediateData[]
//
// Reference: IEEE 1394-1995 §6.2, Linux kernel drivers/firewire/packet-header-definitions.h
//
// Packet Structure (all fields in network byte order / big-endian):
//
// Quadlet 0: [destination_ID:16][tLabel:6][retry:2][tCode:4][priority:4]
// Quadlet 1: [source_ID:16][destination_offset_high:16]
// Quadlet 2: [destination_offset_low:32]
// Quadlet 3 (block/lock): [data_length:16][extended_tcode:16]

// Quadlet 0 field positions (IEEE 1394-1995 §6.2.4)
constexpr uint32_t kIEEE1394_DestinationIDShift = 16;
constexpr uint32_t kIEEE1394_DestinationIDMask  = 0xFFFF0000u;

constexpr uint32_t kIEEE1394_TLabelShift = 10;
constexpr uint32_t kIEEE1394_TLabelMask  = 0x0000FC00u;

constexpr uint32_t kIEEE1394_RetryShift = 8;
constexpr uint32_t kIEEE1394_RetryMask  = 0x00000300u;

constexpr uint32_t kIEEE1394_TCodeShift = 4;
constexpr uint32_t kIEEE1394_TCodeMask  = 0x000000F0u;

constexpr uint32_t kIEEE1394_PriorityShift = 0;
constexpr uint32_t kIEEE1394_PriorityMask  = 0x0000000Fu;

// Quadlet 1 field positions
constexpr uint32_t kIEEE1394_SourceIDShift = 16;
constexpr uint32_t kIEEE1394_SourceIDMask  = 0xFFFF0000u;

constexpr uint32_t kIEEE1394_OffsetHighShift = 0;
constexpr uint32_t kIEEE1394_OffsetHighMask  = 0x0000FFFFu;

// Quadlet 3 field positions (block/lock packets)
constexpr uint32_t kIEEE1394_DataLengthShift = 16;
constexpr uint32_t kIEEE1394_DataLengthMask  = 0xFFFF0000u;

constexpr uint32_t kIEEE1394_ExtendedTCodeShift = 0;
constexpr uint32_t kIEEE1394_ExtendedTCodeMask  = 0x0000FFFFu;

// Transaction codes (IEEE 1394-1995 Table 3-2)
constexpr uint8_t kIEEE1394_TCodeWriteQuadRequest   = 0x0;
constexpr uint8_t kIEEE1394_TCodeWriteBlockRequest  = 0x1;
constexpr uint8_t kIEEE1394_TCodeWriteResponse      = 0x2;
constexpr uint8_t kIEEE1394_TCodeReadQuadRequest    = 0x4;
constexpr uint8_t kIEEE1394_TCodeReadBlockRequest   = 0x5;
constexpr uint8_t kIEEE1394_TCodeReadQuadResponse   = 0x6;
constexpr uint8_t kIEEE1394_TCodeReadBlockResponse  = 0x7;
constexpr uint8_t kIEEE1394_TCodeCycleStart         = 0x8;
constexpr uint8_t kIEEE1394_TCodeLockRequest        = 0x9;
constexpr uint8_t kIEEE1394_TCodeIsochronousBlock   = 0xA;
constexpr uint8_t kIEEE1394_TCodeLockResponse       = 0xB;
constexpr uint8_t kIEEE1394_TCodePhyPacket         = 0xE; // Link internal/PHY packet

// Retry codes (IEEE 1394-1995 §6.2.4.3)
constexpr uint8_t kIEEE1394_RetryNew = 0x0;
constexpr uint8_t kIEEE1394_RetryX   = 0x1;  // Exponential backoff
constexpr uint8_t kIEEE1394_RetryA   = 0x2;
constexpr uint8_t kIEEE1394_RetryB   = 0x3;

// Priority values (IEEE 1394-1995 §6.2.4.4)
constexpr uint8_t kIEEE1394_PriorityDefault = 0x0;

// Response codes (IEEE 1394-1995 Table 3-3)
constexpr uint8_t kIEEE1394_RCodeComplete      = 0x0;
constexpr uint8_t kIEEE1394_RCodeConflictError = 0x4;
constexpr uint8_t kIEEE1394_RDataError         = 0x5;
constexpr uint8_t kIEEE1394_RCodeTypeError     = 0x6;
constexpr uint8_t kIEEE1394_RCodeAddressError  = 0x7;

} // namespace ASFW::Driver

// (PHY register constants moved to IEEE1394.hpp)
