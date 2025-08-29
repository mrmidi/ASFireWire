// ASOHCICtxRegMap.hpp
#pragma once
#include <stdint.h>
#include "OHCIConstants.hpp"
#include "ASOHCITypes.hpp"   // ASContextKind, ASContextOffsets

// Computes per-context register offsets per OHCI 1.1 Register Map.
// - AT: fixed 4 contexts at 0x0180..0x01FF (ReqTx, RspTx, ReqRx, RspRx).
// - IT: N in [0..31] at 0x0200 + 16*N.
// - IR: N in [0..31] at 0x0400 + 32*N (provided for completeness).
//
// Read ContextControl at 'contextBase'.
// Write-1-to-set uses 'contextControlSet' (== contextBase).
// Write-1-to-clear uses 'contextControlClear' (base + 0x04).
// CommandPtr is at base + 0x0C.
// Sources: OHCI 1.1 §4.2 Table 4-3 (addresses for Async/IT/IR contexts).  [oai_citation:1‡42-register-map.pdf](file-service://file-P79PQ5V6pmT1XpGoSNzZeP)
class ASOHCICtxRegMap final {
public:
    static constexpr uint32_t kATReqTxBase = 0x0180; // Async request transmit
    static constexpr uint32_t kATRspTxBase = 0x01A0; // Async response transmit
    static constexpr uint32_t kATReqRxBase = 0x01C0; // Async request receive
    static constexpr uint32_t kATRspRxBase = 0x01E0; // Async response receive

    static constexpr uint32_t kITBase0     = 0x0200; // Isoch transmit context 0
    static constexpr uint32_t kITStride    = 0x0010; // +16 bytes per IT context

    static constexpr uint32_t kIRBase0     = 0x0400; // Isoch receive context 0
    static constexpr uint32_t kIRStride    = 0x0020; // +32 bytes per IR context

    // Returns false on invalid (e.g., IT index >= 32).
    static bool Compute(ASContextKind kind, uint32_t index, ASContextOffsets* out) {
        if (!out) return false;
        uint32_t base = 0;

        switch (kind) {
        case ASContextKind::kAT_Request:   // §4.2: 0x0180 block; Set=+0x08, Clear=+0x04, CmdPtr=+0x0C.
            base = kATReqTxBase;
            break;
        case ASContextKind::kAT_Response:  // §4.2: 0x01A0 block; Set=+0x08, Clear=+0x04, CmdPtr=+0x0C.
            base = kATRspTxBase;
            break;
        case ASContextKind::kAR_Request:   // §4.2: 0x01C0 block; Set=+0x08, Clear=+0x04, CmdPtr=+0x0C.
            base = kATReqRxBase;
            break;
        case ASContextKind::kAR_Response:  // §4.2: 0x01E0 block; Set=+0x08, Clear=+0x04, CmdPtr=+0x0C.
            base = kATRspRxBase;
            break;
        case ASContextKind::kIT_Transmit:  // §4.2: 0x0200 + 16*n; Set=base, Clear=base+0x04, CmdPtr=base+0x0C.
            if (index >= 32) return false;
            base = kITBase0 + kITStride * index;
            break;
        case ASContextKind::kIR_Receive:   // §4.2: 0x0400 + 32*n; Set=base, Clear=base+0x04, CmdPtr=base+0x0C.
            if (index >= 32) return false;
            base = kIRBase0 + kIRStride * index;
            break;
        default:
            return false;
        }

        // Read address is always the "ContextControl" (base). Set/Clear vary:
        out->contextBase = base;                // read ContextControl
        if (kind == ASContextKind::kIT_Transmit || kind == ASContextKind::kIR_Receive) {
            out->contextControlSet   = base;        // IT/IR: Set at base
            out->contextControlClear = base + 0x04; // IT/IR: Clear at +0x04
            out->commandPtr          = base + 0x0C; // IT/IR: CommandPtr at +0x0C
        } else {
            out->contextControlSet   = base + 0x08; // AT/AR: Set at +0x08
            out->contextControlClear = base + 0x04; // AT/AR: Clear at +0x04
            out->commandPtr          = base + 0x0C; // AT/AR: CommandPtr at +0x0C
        }
        return true;
    }
};