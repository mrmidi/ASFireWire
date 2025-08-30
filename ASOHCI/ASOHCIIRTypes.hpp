#pragma once
//
// ASOHCIIRTypes.hpp
// IR (Isochronous Receive) enums and option structs
//
// Spec refs (OHCI 1.1): §10.1 (IR DMA Context Programs), §10.2 (Receive Modes),
//   §10.3 (IR Context Registers), §10.5 (IR Interrupts), §10.6 (IR Data Formats).
//   Chapter 6 for global IntEvent / IsoRxIntEvent bit demux.

#include <stdint.h>

// IR receive modes (OHCI §10.2)
enum class IRMode : uint8_t {
    kBufferFill = 0,        // §10.2.1: Concatenate packets into contiguous stream
    kPacketPerBuffer = 1,   // §10.2.2: Each packet in separate descriptor block  
    kDualBuffer = 2,        // §10.2.3: Split payload into two buffer streams
};

// IR interrupt policy for INPUT_LAST* descriptors 'i' field (OHCI §10.1.1, Table 10-1)
enum class IRIntPolicy : uint8_t {
    kNever  = 0,    // i=00: No interrupt on completion
    kAlways = 3,    // i=11: Interrupt on completion (IsochRx event)
};

// IR synchronization field matching (OHCI §10.3 IRContextMatch)
enum class IRSyncMatch : uint8_t {
    kNoWait = 0,    // w=00: Accept all packets regardless of sync field
    kWaitSync = 3,  // w=11: Wait for packet with matching sync field
};

// Channel filtering options (OHCI §10.3)
struct IRChannelFilter {
    bool     multiChannelMode = false;  // Enable multi-channel reception on context 0
    uint64_t channelMask = 0;           // Bit mask for channels 0-63 (if multiChannelMode)
    uint8_t  singleChannel = 0;         // Single channel for non-multi-channel contexts
    uint8_t  tag = 0;                   // Tag field filter (4 bits)
    uint8_t  sync = 0;                  // Sync field for sync matching (4 bits)
};

// Per-packet receive options
struct IRQueueOptions {
    IRMode          receiveMode = IRMode::kPacketPerBuffer;     // OHCI §10.2 receive mode
    IRIntPolicy     interruptPolicy = IRIntPolicy::kNever;     // §10.1.1 interrupt control
    IRSyncMatch     syncMatch = IRSyncMatch::kNoWait;          // §10.1.1 wait control
    bool            includeHeader = false;                     // Include isochronous header
    bool            includeTimestamp = false;                  // Include timestamp trailer
    
    // Dual-buffer specific options (OHCI §10.2.3)
    uint16_t        firstSize = 8;      // Fixed size for first portion (multiple of 4)
};

// High-level policy for IR context management
struct IRPolicy {
    IRChannelFilter channelFilter{};        // Channel and tag filtering setup
    bool            dropOnOverrun = true;       // Drop packets if buffers full
    uint32_t        bufferWatermarkUs = 1000;   // Buffer fullness threshold (microseconds)
    uint32_t        bufferFillWatermark = 4;    // Refill when N or fewer descriptors free
    bool            headerSplitting = false;    // Include isochronous header in data
    bool            timestampingEnabled = true; // Enable timestamp trailers
    bool            enableErrorLogging = true;  // Log packet errors and drops
};

// IR context status and statistics
struct IRStats {
    uint32_t packetsReceived = 0;       // Total packets received successfully
    uint32_t packetsDropped = 0;        // Packets dropped due to buffer issues
    uint32_t bytesReceived = 0;         // Total payload bytes received
    uint32_t bufferOverruns = 0;        // Buffer-fill overrun events
    uint32_t syncMismatches = 0;        // Packets rejected due to sync field
    uint32_t channelMismatches = 0;     // Packets rejected due to channel filter
};

// Completion callback data for received packets
struct IRCompletion {
    bool     success = false;           // Packet received without errors
    uint8_t  channel = 0;               // Channel number packet was received on
    uint8_t  tag = 0;                   // Tag field from packet header
    uint8_t  sy = 0;                    // Sync field from packet header
    uint16_t dataLength = 0;            // Payload length in bytes
    uint16_t timestamp = 0;             // Cycle timestamp if enabled
    uint16_t status = 0;                // Raw ContextControl status bits
};

// DualBuffer mode payload splitting info (OHCI §10.2.3)
struct IRDualBufferInfo {
    uint16_t firstSize;                 // Size of first portion per packet
    uint32_t firstBufferPA;             // Physical address of first buffer
    uint32_t secondBufferPA;            // Physical address of second buffer
    uint16_t firstReqCount;             // First buffer request count
    uint16_t secondReqCount;            // Second buffer request count
};