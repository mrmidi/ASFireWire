//#pragma once
//
// ASOHCIATTypes.hpp
// Common enums/structs for Asynchronous Transmit
//
// Spec refs: OHCI 1.1 §7 (Asynchronous Transmit DMA), §7.5 (Interrupts), §7.6 (Pipelining), §7.3 (Retries)

#pragma once

#include <stdint.h>

// 1394 ACK summary (driver-facing)
enum class ATAck : uint8_t {
    kComplete,          // §7.5
    kPending,           // §7.5
    kBusy,              // §7.3 (busy classes map here if retry budget exhausted)
    kTardy,             // §7.5
    kDataError,         // §7.5 + §7.2 (data error vs. underrun note)
    kMissing,           // context went dead/flushed (§7.6)
    kFlushed,           // bus reset flushed in-flight (§7.6)
    kUnknown
};

// Hardware “event code” bucket (exact mapping in Status.hpp)
enum class ATEvent : uint8_t {
    kAckComplete,
    kAckPending,
    kAckBusyX, kAckBusyA, kAckBusyB,
    kAckTardy,
    kAckDataError,
    kUnderrun,          // §7.2 (TX FIFO under-run rule)
    kTimeout,           // §7.5
    kTCodeErr,          // §7.7 sanity
    kDataRead,          // §7.7 sanity
    kFlushed,           // §7.6
    kMissingAck,        // §7.6
    kUnknown
};

// Interrupt policy encoded in OUTPUT_LAST* ‘i’ bits (§7.5)
enum class ATIntPolicy : uint8_t {
    kInterestingOnly,   // i=01: interrupt on non-complete/pending results
    kAlways,            // i=11
};

// Caller options for queueing a packet
struct ATQueueOptions {
    ATIntPolicy interruptPolicy = ATIntPolicy::kInterestingOnly; // §7.5
    uint8_t     maxSpeedCode = 0;    // 1394 speed field if needed
    uint16_t    maxPayload   = 0;    // bytes (sanity for builder)
    bool        enforceInOrder = false; // if true, manager limits outstanding=1 (§7.6)
};

