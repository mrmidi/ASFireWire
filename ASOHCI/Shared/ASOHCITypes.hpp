#pragma once
//
// ASOHCITypes.hpp
// Common enums & small types used by AT/AR
//
// Spec refs: OHCI 1.1 §3.1 (Context Registers overview), §3.1.2 (CommandPtr format)
//

#include <stdint.h>

enum class ASContextKind : uint8_t {
    kAT_Request,   // AT Req context (Tx)
    kAT_Response,  // AT Rsp context (Tx)
    kAR_Request,   // AR Req context (Rx)
    kAR_Response,  // AR Rsp context (Rx)
    kIT_Transmit   // Isochronous Transmit (Tx)
};

// Context register offsets bundle (per-kind)
struct ASContextOffsets {
    uint32_t contextBase = 0;        // optional: base register address (for debug)
    uint32_t contextControlSet = 0;  // ContextControl.Set
    uint32_t contextControlClear = 0;// ContextControl.Clear
    uint32_t commandPtr = 0;         // CommandPtr
};

// Minimal completion view that both AT/AR can surface from OUTPUT_LAST/INPUT_LAST
// (xferStatus/timeStamp encoding differs by descriptor type; callers decode as needed)
// Spec refs: §3.1.2 (CommandPtr & Z); AT §7.1.5 (completion fields); AR §8.1.5 (completion fields).
struct ASCompletionMini {
    uint16_t xferStatus = 0; // lower 16 of “status/timestamp” quadlet used by the context
    uint16_t timeStamp  = 0; // upper or lower 16 depending on desc type (caller knows side)
};
