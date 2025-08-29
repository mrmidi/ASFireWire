// ASOHCICtxRegMap.hpp
#pragma once
#include <stdint.h>
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
        case ASContextKind::kAT_Request:   // §4.2: 0x0180.. CommandPtr at +0x0C.  [oai_citation:2‡42-register-map.pdf](file-service://file-P79PQ5V6pmT1XpGoSNzZeP)
            base = kATReqTxBase;
            break;
        case ASContextKind::kAT_Response:  // §4.2: 0x01A0.. CommandPtr at +0x0C.  [oai_citation:3‡42-register-map.pdf](file-service://file-P79PQ5V6pmT1XpGoSNzZeP)
            base = kATRspTxBase;
            break;
        case ASContextKind::kAR_Request:   // §4.2: 0x01C0.. CommandPtr at +0x0C.  [oai_citation:4‡42-register-map.pdf](file-service://file-P79PQ5V6pmT1XpGoSNzZeP)
            base = kATReqRxBase;
            break;
        case ASContextKind::kAR_Response:  // §4.2: 0x01E0.. CommandPtr at +0x0C.  [oai_citation:5‡42-register-map.pdf](file-service://file-P79PQ5V6pmT1XpGoSNzZeP)
            base = kATRspRxBase;
            break;
        case ASContextKind::kIT_Transmit:  // §4.2: 0x0200 + 16*n (n=0..31).  [oai_citation:6‡42-register-map.pdf](file-service://file-P79PQ5V6pmT1XpGoSNzZeP)
            if (index >= 32) return false;
            base = kITBase0 + kITStride * index;
            break;
        default:
            return false;
        }

        out->contextBase        = base;         // read ContextControl
        out->contextControlSet  = base;         // write-1-to-set
        out->contextControlClear= base + 0x04;  // write-1-to-clear
        out->commandPtr         = base + 0x0C;  // CommandPtr
        return true;
    }
};